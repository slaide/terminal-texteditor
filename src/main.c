#define _GNU_SOURCE
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
#include "lsp.h"
#include "editor_config.h"

static long frame_remaining_ms(struct timespec *last_frame, int target_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - last_frame->tv_sec) * 1000L +
                      (now.tv_nsec - last_frame->tv_nsec) / 1000000L;
    long remaining = target_ms - elapsed_ms;
    return remaining > 0 ? remaining : 0;
}

static bool frame_due(struct timespec *last_frame, int target_ms) {
    long remaining = frame_remaining_ms(last_frame, target_ms);
    if (remaining == 0) {
        clock_gettime(CLOCK_MONOTONIC, last_frame);
        return true;
    }
    return false;
}

// Per-line diagnostic info for rendering
typedef struct {
    int line;              // 0-based line number
    DiagnosticSeverity severity;
    char *message;
    char *source;
} LineDiagnostic;

// Stored semantic token for syntax highlighting
typedef struct {
    int line;
    int col;
    int length;
    SemanticTokenType type;
} StoredToken;

// Code folding
typedef struct {
    int start_line;     // Line that remains visible (0-based)
    int end_line;       // Last hidden line (inclusive)
    bool is_folded;     // Currently collapsed
} Fold;

// FoldStyle is defined in editor_config.h as ConfigFoldStyle

typedef struct {
    char *name;
    bool is_dir;
    long size;
} FileEntry;

typedef struct {
    char *data;
    int len;
    int cap;
} RenderBuf;

typedef struct {
    TextBuffer *buffer;
    int cursor_x, cursor_y;
    int offset_x, offset_y;
    int select_start_x, select_start_y;
    int select_end_x, select_end_y;
    bool selecting;
    bool modified;
    char *filename;
    time_t file_mtime;  // Last known modification time of the file
    int last_cursor_x, last_cursor_y;
    int last_offset_x, last_offset_y;

    // LSP diagnostics for this file
    LineDiagnostic *diagnostics;
    int diagnostic_count;
    int diagnostic_capacity;
    bool lsp_opened;  // Whether we've sent didOpen to LSP
    int lsp_version;
    char *lsp_name;

    // Semantic tokens for syntax highlighting
    StoredToken *tokens;
    int token_count;
    int token_capacity;
    int *token_line_start;
    int *token_line_count;
    int token_line_capacity;

    // Code folding
    Fold *folds;
    int fold_count;
    int fold_capacity;
    ConfigFoldStyle fold_style;
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
    FileEntry *file_list;
    int file_count;
    int file_capacity;
    int file_manager_cursor;
    int file_manager_offset;
    bool file_manager_focused;
    
    // Quit confirmation dialog state
    bool quit_confirmation_active;
    
    // File reload confirmation dialog state
    bool reload_confirmation_active;
    int reload_tab_index;  // Which tab needs reloading

    // LSP state
    bool lsp_enabled;
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
void draw_tab_bar(RenderBuf *rb);
void switch_to_next_tab(void);
void switch_to_prev_tab(void);
void enter_filename_input_mode(void);
void exit_filename_input_mode(void);
void process_filename_input(void);
void clear_selection(void);
void toggle_file_manager(void);
void refresh_file_list(void);
void draw_file_manager(RenderBuf *rb);
void file_manager_navigate(int direction);
void file_manager_select_item(void);
void free_file_list(void);
const char* get_file_size_str(long size, bool is_dir);
bool is_directory(const char* filepath);
bool has_unsaved_changes(void);
void show_quit_confirmation(void);
void draw_quit_confirmation(RenderBuf *rb);
int find_tab_with_file(const char* filename);
time_t get_file_mtime(const char* filename);
void check_file_changes(void);
void show_reload_confirmation(int tab_index);
void draw_reload_confirmation(RenderBuf *rb);
void reload_file_in_tab(int tab_index);
void draw_modal(RenderBuf *rb, const char* title, const char* message, const char* bg_color, const char* fg_color);
void lsp_diagnostics_handler(const char *uri, Diagnostic *diags, int count);
void clear_tab_diagnostics(Tab *tab);
void notify_lsp_file_opened(Tab *tab);
void notify_lsp_file_changed(Tab *tab);
void notify_lsp_file_closed(Tab *tab);
char *get_buffer_content(TextBuffer *buffer);
DiagnosticSeverity get_line_diagnostic_severity(Tab *tab, int line);
const char *get_line_diagnostic_message(Tab *tab, int line);
void lsp_semantic_tokens_handler(const char *uri, SemanticToken *tokens, int count);
void clear_tab_tokens(Tab *tab);
void request_semantic_tokens(Tab *tab);
const char *get_token_color(SemanticTokenType type);

// Code folding functions
void clear_tab_folds(Tab *tab);
void detect_folds(Tab *tab);
// get_fold_style_for_file replaced by editor_config_get_fold_style()
void detect_folds_braces(Tab *tab);
void detect_folds_indent(Tab *tab);
void detect_folds_headings(Tab *tab);
Fold *get_fold_at_line(Tab *tab, int line);
Fold *get_fold_containing_line(Tab *tab, int line);
bool is_line_visible(Tab *tab, int line);
int get_next_visible_line(Tab *tab, int line);
int get_prev_visible_line(Tab *tab, int line);
void toggle_fold_at_line(Tab *tab, int line);
void add_fold(Tab *tab, int start_line, int end_line);
int count_folded_lines_before(Tab *tab, int line);
int file_line_to_display_line(Tab *tab, int file_line);
int display_line_to_file_line(Tab *tab, int display_line);

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
    tab->lsp_version = 1;
    
    tab->buffer = buffer_create();
    if (!tab->buffer) return -1;
    
    if (filename) {
        tab->filename = strdup(filename);
        if (!buffer_load_from_file(tab->buffer, tab->filename)) {
            buffer_insert_line(tab->buffer, 0, "");
        }
        // Initialize file modification time for change tracking
        tab->file_mtime = get_file_mtime(tab->filename);
        // Detect foldable regions
        detect_folds(tab);
    } else {
        buffer_insert_line(tab->buffer, 0, "");
        tab->file_mtime = 0; // No file yet
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
    if (tab->lsp_name) {
        free(tab->lsp_name);
        tab->lsp_name = NULL;
    }
    clear_tab_diagnostics(tab);
    clear_tab_tokens(tab);
    clear_tab_folds(tab);
}

void close_tab(int tab_index) {
    if (tab_index < 0 || tab_index >= editor.tab_count) return;

    // Don't close the last tab
    if (editor.tab_count <= 1) return;

    // Notify LSP that we're closing this file
    notify_lsp_file_closed(&editor.tabs[tab_index]);

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

    // Ensure the file is opened in LSP
    Tab *tab = get_current_tab();
    if (tab) {
        notify_lsp_file_opened(tab);
    }
}

void cleanup_and_exit(int status) {
    // Shutdown LSP first
    if (editor.lsp_enabled) {
        lsp_shutdown();
    }
    editor_config_free();

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

        // Skip over folded lines
        if (dy > 0) {
            // Moving down: if new_y is inside a fold, skip to after the fold
            while (new_y < tab->buffer->line_count && !is_line_visible(tab, new_y)) {
                new_y++;
            }
            if (new_y >= tab->buffer->line_count) {
                new_y = tab->buffer->line_count - 1;
                // If still not visible, find last visible line
                while (new_y > 0 && !is_line_visible(tab, new_y)) {
                    new_y--;
                }
            }
        } else if (dy < 0) {
            // Moving up: if new_y is inside a fold, skip to the fold start line
            Fold *fold = get_fold_containing_line(tab, new_y);
            if (fold) {
                new_y = fold->start_line;
            }
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
                int new_y = get_prev_visible_line(tab, tab->cursor_y);
                if (new_y >= 0) {
                    tab->cursor_y = new_y;
                    int prev_line_len = tab->buffer->lines[tab->cursor_y] ?
                                       strlen(tab->buffer->lines[tab->cursor_y]) : 0;
                    tab->cursor_x = prev_line_len;
                }
            }
        } else if (dx > 0 && tab->cursor_x >= line_len) {
            if (tab->cursor_y < tab->buffer->line_count - 1) {
                int new_y = get_next_visible_line(tab, tab->cursor_y);
                if (new_y < tab->buffer->line_count) {
                    tab->cursor_y = new_y;
                    tab->cursor_x = 0;
                }
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

    // If cursor is above visible area, scroll up
    if (tab->cursor_y < tab->offset_y) {
        tab->offset_y = tab->cursor_y;
    }

    // Count visible lines from offset_y to cursor_y to check if cursor is below visible area
    int visible_rows = editor.screen_rows - 2;
    int display_row = 0;
    int file_y = tab->offset_y;
    while (file_y < tab->cursor_y && display_row < visible_rows) {
        if (is_line_visible(tab, file_y)) {
            display_row++;
        }
        file_y++;
    }

    // If cursor is at or beyond the visible area, scroll down
    if (display_row >= visible_rows) {
        // Find new offset that puts cursor near bottom of screen
        int target_row = visible_rows - 1;
        int new_offset = tab->cursor_y;
        int rows_counted = 0;
        while (new_offset > 0 && rows_counted < target_row) {
            new_offset--;
            if (is_line_visible(tab, new_offset)) {
                rows_counted++;
            }
        }
        tab->offset_y = new_offset;
    }

    // Horizontal scrolling
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

// Helper to find the token type at a given position (file coordinates)
static SemanticTokenType get_token_at(Tab *tab, int line, int col) {
    if (!tab || !tab->tokens || !tab->token_line_start || !tab->token_line_count) {
        return TOKEN_UNKNOWN;
    }
    if (line < 0 || line >= tab->token_line_capacity) return TOKEN_UNKNOWN;

    int start = tab->token_line_start[line];
    int count = tab->token_line_count[line];
    if (start < 0 || count <= 0) return TOKEN_UNKNOWN;

    for (int i = start; i < start + count; i++) {
        if (col >= tab->tokens[i].col &&
            col < tab->tokens[i].col + tab->tokens[i].length) {
            return tab->tokens[i].type;
        }
    }
    return TOKEN_UNKNOWN;
}

static void build_token_line_index(Tab *tab) {
    if (!tab || !tab->buffer || tab->token_count == 0 || !tab->tokens) return;

    int line_count = tab->buffer->line_count;
    if (line_count <= 0) return;

    if (tab->token_line_capacity < line_count) {
        int *new_start = malloc(sizeof(int) * line_count);
        int *new_count = malloc(sizeof(int) * line_count);
        if (!new_start || !new_count) {
            free(new_start);
            free(new_count);
            return;
        }
        free(tab->token_line_start);
        free(tab->token_line_count);
        tab->token_line_start = new_start;
        tab->token_line_count = new_count;
        tab->token_line_capacity = line_count;
    }

    for (int i = 0; i < line_count; i++) {
        tab->token_line_start[i] = -1;
        tab->token_line_count[i] = 0;
    }

    for (int i = 0; i < tab->token_count; i++) {
        int line = tab->tokens[i].line;
        if (line < 0 || line >= line_count) continue;
        if (tab->token_line_start[line] == -1) {
            tab->token_line_start[line] = i;
        }
        tab->token_line_count[line]++;
    }
}

static void render_buf_init(RenderBuf *rb) {
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static void render_buf_free(RenderBuf *rb) {
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static bool render_buf_ensure(RenderBuf *rb, int extra) {
    if (rb->len + extra + 1 <= rb->cap) return true;
    int new_cap = rb->cap == 0 ? 256 : rb->cap * 2;
    while (new_cap < rb->len + extra + 1) new_cap *= 2;
    char *new_data = realloc(rb->data, new_cap);
    if (!new_data) return false;
    rb->data = new_data;
    rb->cap = new_cap;
    return true;
}

static void render_buf_append(RenderBuf *rb, const char *s) {
    if (!s) return;
    int n = strlen(s);
    if (!render_buf_ensure(rb, n)) return;
    memcpy(rb->data + rb->len, s, n);
    rb->len += n;
    rb->data[rb->len] = '\0';
}

static void render_buf_append_char(RenderBuf *rb, char c) {
    if (!render_buf_ensure(rb, 1)) return;
    rb->data[rb->len++] = c;
    rb->data[rb->len] = '\0';
}

static void render_buf_appendf(RenderBuf *rb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed <= 0) return;
    if (!render_buf_ensure(rb, needed)) return;
    va_start(ap, fmt);
    vsnprintf(rb->data + rb->len, rb->cap - rb->len, fmt, ap);
    va_end(ap);
    rb->len += needed;
}

static void render_move_cursor(RenderBuf *rb, int row, int col) {
    render_buf_appendf(rb, "\033[%d;%dH", row, col);
}

static void render_clear_screen(RenderBuf *rb) {
    render_buf_append(rb, "\033[2J\033[H");
}

static void draw_line_to_buf(RenderBuf *rb, int screen_y, int file_y, int start_col) {
    Tab* tab = get_current_tab();
    if (!tab || !rb) return;

    render_move_cursor(rb, screen_y + 2, start_col);
    render_buf_append(rb, "\033[K");

    if (file_y < tab->buffer->line_count) {
        // Check for diagnostics on this line and color accordingly
        DiagnosticSeverity sev = get_line_diagnostic_severity(tab, file_y);
        const char *line_num_color = STYLE_LINE_NUMBERS; // Default cyan
        if (sev == DIAG_ERROR) {
            line_num_color = FG_RED;
        } else if (sev == DIAG_WARNING) {
            line_num_color = FG_YELLOW;
        } else if (sev == DIAG_INFO || sev == DIAG_HINT) {
            line_num_color = FG_BLUE;
        }

        // Fold indicator
        Fold *fold = get_fold_at_line(tab, file_y);
        if (fold) {
            if (fold->is_folded) {
            render_buf_append(rb, FG_YELLOW "▶" COLOR_RESET);
        } else {
            render_buf_append(rb, FG_CYAN "▼" COLOR_RESET);
        }
    } else {
        render_buf_append(rb, " ");
    }

    char num_buf[32];
    snprintf(num_buf, sizeof(num_buf), "%s%6d" COLOR_RESET " ", line_num_color, file_y + 1);
    render_buf_append(rb, num_buf);

        // Check if this line starts a folded region (reuse fold from above)
        if (fold && fold->is_folded) {
            // Check if any part of the fold is selected
            bool fold_has_selection = false;
            if (tab->selecting) {
                int sel_start_y = tab->select_start_y;
                int sel_end_y = tab->select_end_y;
                if (sel_start_y > sel_end_y) {
                    int tmp = sel_start_y;
                    sel_start_y = sel_end_y;
                    sel_end_y = tmp;
                }
                // Check if selection overlaps with fold region
                if (sel_start_y <= fold->end_line && sel_end_y >= file_y) {
                    fold_has_selection = true;
                }
            }

            // Show abbreviated content with fold indicator
            char *line = tab->buffer->lines[file_y];
            int folded_lines = fold->end_line - fold->start_line;
            int display_len = editor.screen_cols - editor.line_number_width - 20;
            if (display_len < 10) display_len = 10;

            // Apply selection highlighting if needed
            if (fold_has_selection) {
                render_buf_append(rb, "\033[7m");  // Reverse video for selection
            }

            // Print truncated line content
            int printed = 0;
            if (line) {
                for (int i = 0; line[i] && printed < display_len; i++, printed++) {
                    render_buf_append_char(rb, line[i]);
                }
            }

            // Print fold indicator
            char fold_buf[64];
            snprintf(fold_buf, sizeof(fold_buf), FG_YELLOW " ... (%d lines)" COLOR_RESET, folded_lines);
            render_buf_append(rb, fold_buf);
            return;
        }

        if (tab->buffer->lines[file_y]) {
            char *line = tab->buffer->lines[file_y];
            int len = strlen(line);
            int start_x = tab->offset_x;
            int display_len = editor.screen_cols - editor.line_number_width;

            // Calculate selection bounds for this line
            bool line_has_selection = false;
            int sel_start = 0, sel_end = 0;

            if (tab->selecting) {
                int sel_start_x = tab->select_start_x;
                int sel_start_y = tab->select_start_y;
                int sel_end_x = tab->select_end_x;
                int sel_end_y = tab->select_end_y;

                // Normalize selection
                if (sel_start_y > sel_end_y || (sel_start_y == sel_end_y && sel_start_x > sel_end_x)) {
                    int temp_x = sel_start_x, temp_y = sel_start_y;
                    sel_start_x = sel_end_x; sel_start_y = sel_end_y;
                    sel_end_x = temp_x; sel_end_y = temp_y;
                }

                if (file_y >= sel_start_y && file_y <= sel_end_y) {
                    line_has_selection = true;
                    sel_start = (file_y == sel_start_y) ? sel_start_x : 0;
                    sel_end = (file_y == sel_end_y) ? sel_end_x : len;
                }
            }

            // Handle empty line that's selected
            if (line_has_selection && len == 0) {
                render_buf_append(rb, "\033[7m \033[0m");
                return;
            }

            // Calculate visible portion
            int end_x = start_x + display_len;
            if (end_x > len) end_x = len;

            // Render character by character with syntax highlighting
            bool has_tokens = (tab->tokens != NULL && tab->token_count > 0);
            const char *current_color = NULL;
            bool in_selection = false;

            for (int x = start_x; x < end_x; x++) {
                bool char_selected = line_has_selection && x >= sel_start && x < sel_end;

                // Handle selection state change
                if (char_selected != in_selection) {
                    if (char_selected) {
                        render_buf_append(rb, "\033[7m"); // Start reverse video
                    } else {
                        render_buf_append(rb, "\033[27m"); // End reverse video
                    }
                    in_selection = char_selected;
                }

                // Get color for this position (syntax highlighting)
                const char *new_color = NULL;
                if (has_tokens) {
                    SemanticTokenType type = get_token_at(tab, file_y, x);
                    new_color = get_token_color(type);
                }

                // Apply color change if needed
                if (new_color != current_color) {
                    if (new_color) {
                        render_buf_append(rb, new_color);
                    } else {
                        render_buf_append(rb, COLOR_RESET);
                        if (in_selection) render_buf_append(rb, "\033[7m"); // Restore selection
                    }
                    current_color = new_color;
                }

                // Output the character
                render_buf_append_char(rb, line[x]);
            }

            // Reset formatting
            render_buf_append(rb, COLOR_RESET);

            // Show selection extends to end of line
            if (line_has_selection && sel_end >= len && end_x >= len) {
                render_buf_append(rb, "\033[7m \033[0m");
            }
        }
    } else {
        char tilde_buf[32];
        snprintf(tilde_buf, sizeof(tilde_buf), " %s%6s%s ", FG_CYAN, "~", COLOR_RESET);
        render_buf_append(rb, tilde_buf);  // Space for fold indicator column
    }
}

void draw_line(int screen_y, int file_y, int start_col) {
    RenderBuf rb;
    render_buf_init(&rb);
    draw_line_to_buf(&rb, screen_y, file_y, start_col);
    if (rb.data && rb.len > 0) {
        fwrite(rb.data, 1, rb.len, stdout);
    }
    render_buf_free(&rb);
}

void draw_tab_bar(RenderBuf *rb) {
    // Draw "File Browser" label in top-left if file manager is visible
    if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
        render_move_cursor(rb, 1, 1);
        if (editor.file_manager_focused) {
            render_buf_append(rb, "\033[44m\033[1m"); // Blue background, bold when focused
        } else {
            render_buf_append(rb, "\033[100m\033[1m"); // Dark gray background, bold when not focused
        }
        render_buf_append(rb, " File Browser ");
        // Fill remaining width of file manager area
        for (int x = 14; x <= editor.file_manager_width; x++) {
            render_buf_append(rb, " ");
        }
        render_buf_append(rb, "\033[0m"); // Reset formatting
    }
    
    // Calculate tab bar position and width
    int tab_start_col = 1;
    int tab_width = editor.screen_cols;
    
    if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
        tab_start_col += editor.file_manager_width + 1; // Start after file manager
        tab_width -= editor.file_manager_width + 1;     // Reduce width
    }
    
    render_move_cursor(rb, 1, tab_start_col);
    render_buf_append(rb, "\033[K\033[7m"); // Clear line and reverse video
    
    int col = tab_start_col;
    for (int i = 0; i < editor.tab_count; i++) {
        Tab* tab = &editor.tabs[i];
        const char* filename = tab->filename ? tab->filename : "untitled";
        
        // Extract just the filename from path
        const char* basename = filename;
        const char* slash = strrchr(filename, '/');
        if (slash) basename = slash + 1;
        
        // Highlight current tab
        if (i == editor.current_tab) {
            render_buf_append(rb, "\033[0m\033[7m"); // Reverse video for active tab
            render_buf_appendf(rb, " >%d:%s%s< ", i + 1, basename, tab->modified ? "*" : "");
        } else {
            render_buf_append(rb, "\033[0m\033[100m\033[97m"); // Grey background, white text for inactive
            render_buf_appendf(rb, " %d:%s%s ", i + 1, basename, tab->modified ? "*" : "");
        }
        
        if (i == editor.current_tab) {
            col += strlen(basename) + (tab->modified ? 8 : 7); // Account for >, number, colon, spaces, <, and optional *
        } else {
            col += strlen(basename) + (tab->modified ? 6 : 5); // Account for number, colon, spaces, and optional *
        }
        
        if (col >= tab_start_col + tab_width - 10) {
            render_buf_append(rb, "...");
            break;
        }
    }
    
    render_buf_append(rb, "\033[0m"); // Reset formatting
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
        
        // Check if file is already open in a tab
        int existing_tab = find_tab_with_file(editor.filename_input);
        if (existing_tab >= 0) {
            // File is already open, switch to that tab
            switch_to_tab(existing_tab);
            set_status_message("Switched to existing tab %d (%s)", existing_tab + 1, editor.filename_input);
        } else {
            // File not open, create new tab
            int new_tab = create_new_tab(editor.filename_input);
            if (new_tab >= 0) {
                switch_to_tab(new_tab);
                set_status_message("Opened %s in tab %d", editor.filename_input, new_tab + 1);
            } else {
                set_status_message("Error: Could not open file %s", editor.filename_input);
            }
        }
    }
    exit_filename_input_mode();
}

void draw_status_line(RenderBuf *rb) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    render_move_cursor(rb, editor.screen_rows, 1);
    render_buf_append(rb, "\033[K\033[7m ");
    
    if (editor.find_mode) {
        render_buf_appendf(rb, "Find: %s", editor.search_query ? editor.search_query : "");
        if (editor.total_matches > 0) {
            render_buf_appendf(rb, "  [%d/%d]", editor.current_match, editor.total_matches);
        } else if (editor.search_query_len > 0) {
            render_buf_append(rb, "  [no matches]");
        }
        render_buf_append(rb, "  (Ctrl+N: next, Ctrl+P: prev, Esc: exit)");
    } else if (editor.filename_input_mode) {
        render_buf_appendf(rb, "Open file: %s", editor.filename_input ? editor.filename_input : "");
        render_buf_append(rb, "  (Enter: open, Esc: cancel)");
    } else {
        time_t now = time(NULL);
        if (editor.status_message && (now - editor.status_message_time < 3)) {
            render_buf_append(rb, editor.status_message);
        } else {
            // Show comprehensive file information
            const char* filename = tab->filename ? tab->filename : "untitled";
            int current_line = tab->cursor_y + 1; // 1-based line numbers
            int total_lines = tab->buffer->line_count;
            int file_size = get_file_size();
            const char* size_str = format_file_size(file_size);
            const char* modified_str = tab->modified ? " [modified]" : "";
            const char* lsp_str = (tab->lsp_opened && tab->lsp_name) ? tab->lsp_name : "off";
            
            // Check for diagnostic on current line
            const char *diag_msg = get_line_diagnostic_message(tab, tab->cursor_y);

            if (diag_msg) {
                // Show diagnostic message in status bar
                DiagnosticSeverity sev = get_line_diagnostic_severity(tab, tab->cursor_y);
                const char *sev_str = (sev == DIAG_ERROR) ? "error" :
                                      (sev == DIAG_WARNING) ? "warning" :
                                      (sev == DIAG_INFO) ? "info" : "hint";
                render_buf_appendf(rb, "[%s] %s", sev_str, diag_msg);
            } else if (editor.file_manager_visible && editor.file_manager_focused) {
                render_buf_appendf(rb, "%s  Line %d/%d  %s%s  LSP:%s  [FILE MANAGER - Esc to return]",
                                   filename, current_line, total_lines, size_str, modified_str, lsp_str);
            } else {
                render_buf_appendf(rb, "%s  Line %d/%d  %s%s  LSP:%s",
                                   filename, current_line, total_lines, size_str, modified_str, lsp_str);
            }
        }
    }
    
    render_buf_append(rb, " \033[0m");
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

    RenderBuf rb;
    render_buf_init(&rb);
    
    if (editor.needs_full_redraw || offset_changed) {
        if (editor.needs_full_redraw) {
            render_clear_screen(&rb);
        }
        
        // Draw tab bar
        draw_tab_bar(&rb);
        
        // Draw file manager if visible
        if (editor.file_manager_visible) {
            draw_file_manager(&rb);
        }
        
        // Calculate text area position
        int text_start_col = 1;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            text_start_col += editor.file_manager_width + 1; // +1 for border
        }
        
        // Draw content (screen_rows - 2 to account for tab bar and status line)
        // Handle folded lines by skipping invisible ones
        int file_y = tab->offset_y;
        for (int y = 0; y < editor.screen_rows - 2; y++) {
            // Skip folded (invisible) lines
            while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
                file_y++;
            }

            draw_line_to_buf(&rb, y, file_y, text_start_col);
            file_y++;
        }
        
        draw_status_line(&rb);
        editor.needs_full_redraw = false;
        tab->last_offset_x = tab->offset_x;
        tab->last_offset_y = tab->offset_y;
    } else {
        // Just update dynamic parts without full redraw
        if (editor.file_manager_visible) {
            draw_file_manager(&rb);
        }
        draw_status_line(&rb);
    }
    
    // Draw confirmation dialogs if active (overlay on top of everything)
    draw_quit_confirmation(&rb);
    draw_reload_confirmation(&rb);

    if (rb.data && rb.len > 0) {
        fwrite(rb.data, 1, rb.len, stdout);
    }
    render_buf_free(&rb);
    
    // Show/hide cursor based on mode, selection state, and focus
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
    } else if (editor.file_manager_visible && editor.file_manager_focused) {
        // Hide cursor when file manager is focused
        terminal_hide_cursor();
    } else if (!tab->selecting) {
        terminal_show_cursor();

        // Calculate text area starting column (same logic as in draw_screen)
        int text_start_col = 1;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            text_start_col += editor.file_manager_width + 1; // +1 for border
        }

        // Calculate screen row accounting for folded lines
        // Count visible lines from offset_y to cursor_y
        int visible_lines = 0;
        for (int y = tab->offset_y; y < tab->cursor_y; y++) {
            if (is_line_visible(tab, y)) {
                visible_lines++;
            }
        }
        int screen_row = visible_lines + 2;  // +2 for tab bar
        int screen_col = (tab->cursor_x - tab->offset_x) + text_start_col + editor.line_number_width;

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

    // Notify LSP of the change
    notify_lsp_file_changed(tab);

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

// Convert screen row (0-based from content area) to file line, accounting for folds
static int screen_y_to_file_y(Tab *tab, int screen_y) {
    if (!tab) return screen_y;

    int file_y = tab->offset_y;
    int current_screen_y = 0;

    while (current_screen_y < screen_y && file_y < tab->buffer->line_count) {
        // Skip invisible (folded) lines
        while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
            file_y++;
        }
        if (file_y >= tab->buffer->line_count) break;

        if (current_screen_y == screen_y) break;
        current_screen_y++;
        file_y++;
    }

    // Skip any remaining invisible lines
    while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
        file_y++;
    }

    return file_y;
}

void handle_mouse(int button, int x, int y, int pressed) {
    Tab* tab = get_current_tab();
    if (!tab) return;

    // Handle clicks on tab bar
    if (y == 1 && button == 0 && pressed) {
        // Calculate tab bar start position
        int tab_start_col = 1;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            tab_start_col += editor.file_manager_width + 1;
        }

        // Find which tab was clicked
        int col = tab_start_col;
        for (int i = 0; i < editor.tab_count; i++) {
            Tab* t = &editor.tabs[i];
            const char* filename = t->filename ? t->filename : "untitled";
            const char* basename = filename;
            const char* slash = strrchr(filename, '/');
            if (slash) basename = slash + 1;

            // Calculate tab width based on format:
            // Active:   " >N:basename< " or " >N:basename*< "
            // Inactive: " N:basename "   or " N:basename* "
            int tab_width;
            int num_digits = (i + 1 >= 10) ? 2 : 1;
            if (i == editor.current_tab) {
                // " >" (2) + num + ":" (1) + basename + maybe "*" + "< " (2)
                tab_width = 5 + num_digits + strlen(basename) + (t->modified ? 1 : 0);
            } else {
                // " " (1) + num + ":" (1) + basename + maybe "*" + " " (1)
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

    // Ignore other clicks on tab bar area
    if (y <= 1) return;

    // Calculate file manager boundary
    int file_manager_end = 0;
    if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
        file_manager_end = editor.file_manager_width + 1; // +1 for border
    }

    // Check if click is in file manager area
    if (editor.file_manager_visible && x <= file_manager_end) {
        if (button == 0 && pressed) {
            // Focus file manager
            if (!editor.file_manager_focused) {
                editor.file_manager_focused = true;
                editor.needs_full_redraw = true;
            }
            // Handle click to select item in file manager
            int clicked_index = (y - 2) + editor.file_manager_offset;
            if (clicked_index >= 0 && clicked_index < editor.file_count) {
                editor.file_manager_cursor = clicked_index;
                editor.needs_full_redraw = true;
            }
        }
        return;
    }

    // Click is in editor area - focus editor if file manager was focused
    if (editor.file_manager_focused && button == 0 && pressed) {
        editor.file_manager_focused = false;
        editor.needs_full_redraw = true;
    }

    // Calculate editor area offset
    int editor_x_offset = file_manager_end;

    // Handle clicks on fold indicator (first column of line number area)
    if (x == editor_x_offset + 1 && button == 0 && pressed) {
        // Calculate which file line was clicked
        int screen_y = y - 2;  // Adjust for tab bar
        if (screen_y >= 0 && screen_y < editor.screen_rows - 2) {
            // Find the file line at this screen position (accounting for folds)
            int file_y = tab->offset_y;
            for (int sy = 0; sy < screen_y && file_y < tab->buffer->line_count; sy++) {
                while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
                    file_y++;
                }
                file_y++;
            }
            // Skip invisible lines to get to the actual displayed line
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

    // Ignore other clicks on line number area
    if (x <= editor_x_offset + editor.line_number_width) {
        return;
    }

    if (button == 0) {  // Left mouse button
        if (pressed) {
            // Mouse button pressed - prepare for potential drag operation
            int buffer_x = x - editor_x_offset - editor.line_number_width - 1 + tab->offset_x;
            int screen_row = y - 2;  // -2 for tab bar
            int buffer_y = screen_y_to_file_y(tab, screen_row);

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
            int buffer_x = x - editor_x_offset - editor.line_number_width - 1 + tab->offset_x;
            int screen_row = y - 2;  // -2 for tab bar
            int buffer_y = screen_y_to_file_y(tab, screen_row);

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
        // Update file modification time after saving
        tab->file_mtime = get_file_mtime(tab->filename);
        set_status_message("File saved: %s", tab->filename);

        // Request updated semantic tokens for syntax highlighting
        request_semantic_tokens(tab);
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

    // Notify LSP of the change
    notify_lsp_file_changed(tab);

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

        // Notify LSP of the change
        notify_lsp_file_changed(tab);

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

        // Notify LSP of the change
        notify_lsp_file_changed(tab);

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

    // Notify LSP of the change
    notify_lsp_file_changed(tab);

    editor.needs_full_redraw = true;
}

bool is_directory(const char* filepath) {
    struct stat statbuf;
    if (stat(filepath, &statbuf) != 0) {
        return false;
    }
    return S_ISDIR(statbuf.st_mode);
}

bool has_unsaved_changes(void) {
    for (int i = 0; i < editor.tab_count; i++) {
        if (editor.tabs[i].modified) {
            return true;
        }
    }
    return false;
}

int find_tab_with_file(const char* filename) {
    if (!filename) return -1;
    
    // Resolve the input filename to absolute path
    char *abs_path = realpath(filename, NULL);
    if (!abs_path) {
        // If realpath fails (file doesn't exist yet), fall back to simple comparison
        for (int i = 0; i < editor.tab_count; i++) {
            if (editor.tabs[i].filename && strcmp(filename, editor.tabs[i].filename) == 0) {
                return i;
            }
        }
        return -1;
    }
    
    // Compare with absolute paths of all open tabs
    for (int i = 0; i < editor.tab_count; i++) {
        if (editor.tabs[i].filename) {
            char *tab_abs_path = realpath(editor.tabs[i].filename, NULL);
            if (tab_abs_path) {
                bool match = (strcmp(abs_path, tab_abs_path) == 0);
                free(tab_abs_path);
                if (match) {
                    free(abs_path);
                    return i;
                }
            } else {
                // If realpath fails for tab filename, compare with original paths
                if (strcmp(filename, editor.tabs[i].filename) == 0) {
                    free(abs_path);
                    return i;
                }
            }
        }
    }
    
    free(abs_path);
    return -1; // Not found
}

time_t get_file_mtime(const char* filename) {
    if (!filename) return 0;
    
    struct stat statbuf;
    if (stat(filename, &statbuf) == 0) {
        return statbuf.st_mtime;
    }
    return 0; // File doesn't exist or error
}

void check_file_changes(void) {
    for (int i = 0; i < editor.tab_count; i++) {
        Tab* tab = &editor.tabs[i];
        if (tab->filename && tab->file_mtime > 0) {
            time_t current_mtime = get_file_mtime(tab->filename);
            if (current_mtime > 0 && current_mtime != tab->file_mtime) {
                // File has been modified externally
                show_reload_confirmation(i);
                break; // Only show one dialog at a time
            }
        }
    }
}

void show_quit_confirmation(void) {
    editor.quit_confirmation_active = true;
    editor.needs_full_redraw = true;
}

void draw_quit_confirmation(RenderBuf *rb) {
    if (!editor.quit_confirmation_active) return;
    
    draw_modal(rb, "You have unsaved changes!", 
               "Press 'q' to quit anyway, or any other key to cancel",
               STYLE_QUIT_DIALOG, 
               FG_WHITE);
}

void show_reload_confirmation(int tab_index) {
    editor.reload_confirmation_active = true;
    editor.reload_tab_index = tab_index;
    editor.needs_full_redraw = true;
}

void draw_reload_confirmation(RenderBuf *rb) {
    if (!editor.reload_confirmation_active) return;
    
    Tab* tab = &editor.tabs[editor.reload_tab_index];
    if (!tab || !tab->filename) return;
    
    // Get just the filename for display
    const char* filename = tab->filename;
    const char* basename = strrchr(filename, '/');
    if (basename) basename++; else basename = filename;
    
    // Construct the message with proper newlines
    static char message[512];
    if (tab->modified) {
        snprintf(message, sizeof(message), 
                "File: %s\n\nWarning: You have unsaved changes!\n\n'r' to reload, any other key to keep current version", 
                basename);
    } else {
        snprintf(message, sizeof(message), 
                "File: %s\n\nThe file has been modified outside the editor.\n\n'r' to reload, any other key to keep current version", 
                basename);
    }
    
    draw_modal(rb, "File Changed Externally!", 
               message,
               STYLE_RELOAD_DIALOG, 
               FG_BLACK);
}

void reload_file_in_tab(int tab_index) {
    if (tab_index < 0 || tab_index >= editor.tab_count) return;
    
    Tab* tab = &editor.tabs[tab_index];
    if (!tab || !tab->filename) return;
    
    // Create new buffer with the updated file content
    TextBuffer* new_buffer = buffer_create();
    if (!new_buffer) return;
    
    if (buffer_load_from_file(new_buffer, tab->filename)) {
        // Successfully loaded, replace the old buffer
        buffer_free(tab->buffer);
        tab->buffer = new_buffer;
        tab->modified = false;
        tab->file_mtime = get_file_mtime(tab->filename);

        // Re-detect folds
        detect_folds(tab);

        // Reset cursor position to top of file
        tab->cursor_x = 0;
        tab->cursor_y = 0;
        tab->offset_x = 0;
        tab->offset_y = 0;

        // Clear selection
        tab->selecting = false;

        set_status_message("File reloaded: %s", tab->filename);
        editor.needs_full_redraw = true;
    } else {
        // Failed to reload
        buffer_free(new_buffer);
        set_status_message("Error: Could not reload file %s", tab->filename);
    }
}

void draw_modal(RenderBuf *rb, const char* title, const char* message, const char* bg_color, const char* fg_color) {
    if (!title || !message || !bg_color || !fg_color) return;
    
    // Calculate dialog dimensions based on terminal size
    int min_width = 40;
    int max_width = editor.screen_cols - 4;
    int dialog_width = min_width;
    
    if (max_width < min_width) {
        dialog_width = max_width;
    } else if (max_width >= 60) {
        dialog_width = 55;
    } else {
        dialog_width = max_width;
    }
    
    // Calculate needed height based on newlines in message
    int line_count = 1; // Start with 1 for the message itself
    for (const char* p = message; *p; p++) {
        if (*p == '\n') line_count++;
    }
    
    int dialog_height = 3 + line_count + 2; // Title + empty line + message lines + padding
    if (dialog_height > editor.screen_rows - 2) {
        dialog_height = editor.screen_rows - 2; // Don't exceed screen
    }
    int start_row = (editor.screen_rows - dialog_height) / 2;
    int start_col = (editor.screen_cols - dialog_width) / 2;
    
    // Ensure dialog fits on screen
    if (start_col < 1) start_col = 1;
    if (start_row < 1) start_row = 1;
    if (start_row + dialog_height > editor.screen_rows) {
        start_row = editor.screen_rows - dialog_height;
    }
    
    // Draw dialog background
    for (int y = 0; y < dialog_height; y++) {
        render_move_cursor(rb, start_row + y, start_col);
        render_buf_append(rb, bg_color);
        for (int x = 0; x < dialog_width; x++) {
            render_buf_append(rb, " ");
        }
    }
    
    int available_width = dialog_width - 4; // Account for 2-char padding on each side
    
    // Draw title (line 1) - always bold
    render_move_cursor(rb, start_row + 1, start_col + 2);
    render_buf_appendf(rb, "%s%s" COLOR_BOLD, bg_color, fg_color);
    if ((int)strlen(title) <= available_width) {
        render_buf_append(rb, title);
    } else {
        render_buf_appendf(rb, "%.*s", available_width, title);
    }
    
    // Empty line (line 2) - just skip it for spacing
    
    // Draw message with newline support (line 3+)
    char* message_copy = strdup(message); // Make a copy we can modify
    if (!message_copy) return;
    
    char* line = message_copy;
    int current_row = 3;
    
    while (line && current_row < dialog_height - 1) { // Leave room for bottom margin
        // Find next newline or end of string
        char* next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0'; // Temporarily terminate this line
            next_line++; // Point to start of next line
        }
        
        // Position cursor for this line
        render_move_cursor(rb, start_row + current_row, start_col + 2);
        render_buf_appendf(rb, "%s%s" COLOR_NORMAL, bg_color, fg_color); // Explicitly normal weight
        
        // Handle line wrapping if this line is too long
        int line_len = strlen(line);
        if (line_len <= available_width) {
            // Line fits, print it
            render_buf_append(rb, line);
        } else {
            // Line is too long, wrap it
            int break_pos = available_width;
            
            // Look for space to break on (work backwards from available_width)
            for (int i = available_width - 1; i > available_width / 2; i--) {
                if (i < line_len && line[i] == ' ') {
                    break_pos = i;
                    break;
                }
            }
            
            // Print first part
            render_buf_appendf(rb, "%.*s", break_pos, line);
            
            // If there's more text and room for another line, print the rest
            if (break_pos < line_len && current_row < dialog_height - 2) {
                current_row++;
                render_move_cursor(rb, start_row + current_row, start_col + 2);
                render_buf_appendf(rb, "%s%s" COLOR_NORMAL, bg_color, fg_color); // Explicitly normal weight
                
                // Skip leading space if we broke on one
                int remaining_start = break_pos;
                if (remaining_start < line_len && line[remaining_start] == ' ') {
                    remaining_start++;
                }
                
                if (remaining_start < line_len) {
                    int remaining_len = line_len - remaining_start;
                    if (remaining_len <= available_width) {
                        render_buf_append(rb, line + remaining_start);
                    } else {
                        render_buf_appendf(rb, "%.*s", available_width, line + remaining_start);
                    }
                }
            }
        }
        
        current_row++;
        line = next_line;
    }
    
    free(message_copy);
    
    render_buf_append(rb, COLOR_RESET); // Reset formatting
}

// LSP Helper Functions

char *get_buffer_content(TextBuffer *buffer) {
    if (!buffer) return NULL;

    // Calculate total size needed
    int total_size = 0;
    for (int i = 0; i < buffer->line_count; i++) {
        if (buffer->lines[i]) {
            total_size += strlen(buffer->lines[i]);
        }
        total_size++; // For newline
    }

    char *content = malloc(total_size + 1);
    if (!content) return NULL;

    int pos = 0;
    for (int i = 0; i < buffer->line_count; i++) {
        if (buffer->lines[i]) {
            int len = strlen(buffer->lines[i]);
            memcpy(content + pos, buffer->lines[i], len);
            pos += len;
        }
        content[pos++] = '\n';
    }
    content[pos] = '\0';

    return content;
}

void clear_tab_diagnostics(Tab *tab) {
    if (!tab) return;
    if (tab->diagnostics) {
        for (int i = 0; i < tab->diagnostic_count; i++) {
            free(tab->diagnostics[i].message);
            free(tab->diagnostics[i].source);
        }
        free(tab->diagnostics);
        tab->diagnostics = NULL;
    }
    tab->diagnostic_count = 0;
    tab->diagnostic_capacity = 0;
}

void lsp_diagnostics_handler(const char *uri, Diagnostic *diags, int count) {
    if (!uri) return;

    // Convert URI to path
    char *path = lsp_uri_to_path(uri);
    if (!path) return;

    // Find the tab with this file
    int tab_idx = find_tab_with_file(path);
    free(path);

    if (tab_idx < 0) return;

    Tab *tab = &editor.tabs[tab_idx];

    // Clear old diagnostics
    clear_tab_diagnostics(tab);

    if (count == 0) {
        editor.needs_full_redraw = true;
        return;
    }

    bool is_markdown = false;
    if (tab->filename) {
        const char *ext = strrchr(tab->filename, '.');
        if (ext && (strcasecmp(ext, ".md") == 0 || strcasecmp(ext, ".markdown") == 0)) {
            is_markdown = true;
        }
    }

    // Store new diagnostics (filter noisy clangd errors for markdown)
    tab->diagnostics = calloc(count, sizeof(LineDiagnostic));
    if (!tab->diagnostics) return;

    tab->diagnostic_capacity = count;
    tab->diagnostic_count = 0;

    for (int i = 0; i < count; i++) {
        if (is_markdown) {
            if (diags[i].source && strcmp(diags[i].source, "clangd") == 0) {
                continue;
            }
            if (diags[i].message &&
                strstr(diags[i].message, "expected exactly one compiler job") != NULL) {
                continue;
            }
        }
        tab->diagnostics[tab->diagnostic_count].line = diags[i].line;
        tab->diagnostics[tab->diagnostic_count].severity = diags[i].severity;
        if (diags[i].message) {
            tab->diagnostics[tab->diagnostic_count].message = strdup(diags[i].message);
        }
        if (diags[i].source) {
            tab->diagnostics[tab->diagnostic_count].source = strdup(diags[i].source);
        }
        tab->diagnostic_count++;
    }

    editor.needs_full_redraw = true;
}

void notify_lsp_file_opened(Tab *tab) {
    if (!tab || !tab->filename || tab->lsp_opened) return;

    // Check if there's an LSP server configured for this file type
    const char *ext = strrchr(tab->filename, '.');
    if (!ext) return;

    LanguageConfig *cfg = editor_config_get_for_extension(ext);
    if (!cfg || !cfg->lsp_command) return;
    const char *command = cfg->lsp_command;

    if (tab->lsp_name) {
        free(tab->lsp_name);
        tab->lsp_name = NULL;
    }
    if (cfg->name) {
        tab->lsp_name = strdup(cfg->name);
    }

    // Initialize (or restart) LSP server for this language
    editor.lsp_enabled = lsp_init(command);
    if (editor.lsp_enabled) {
        lsp_set_diagnostics_callback(lsp_diagnostics_handler);
        lsp_set_semantic_tokens_callback(lsp_semantic_tokens_handler);
    } else {
        return;
    }

    char *content = get_buffer_content(tab->buffer);
    if (content) {
        lsp_did_open(tab->filename, content, cfg->name);
        free(content);
        tab->lsp_opened = true;
        tab->lsp_version = 1;

        // Request semantic tokens for syntax highlighting
        request_semantic_tokens(tab);
    }
}

void notify_lsp_file_changed(Tab *tab) {
    if (!editor.lsp_enabled || !tab || !tab->filename || !tab->lsp_opened) return;

    char *content = get_buffer_content(tab->buffer);
    if (content) {
        tab->lsp_version++;
        lsp_did_change(tab->filename, content, tab->lsp_version);
        free(content);
    }
}

void notify_lsp_file_closed(Tab *tab) {
    if (!editor.lsp_enabled || !tab || !tab->filename || !tab->lsp_opened) return;

    lsp_did_close(tab->filename);
    tab->lsp_opened = false;
    tab->lsp_version = 1;
    if (tab->lsp_name) {
        free(tab->lsp_name);
        tab->lsp_name = NULL;
    }
}

DiagnosticSeverity get_line_diagnostic_severity(Tab *tab, int line) {
    if (!tab || !tab->diagnostics) return 0;

    DiagnosticSeverity worst = 0;
    for (int i = 0; i < tab->diagnostic_count; i++) {
        if (tab->diagnostics[i].line == line) {
            // Lower severity number = worse (1=error is worst)
            if (worst == 0 || tab->diagnostics[i].severity < worst) {
                worst = tab->diagnostics[i].severity;
            }
        }
    }
    return worst;
}

const char *get_line_diagnostic_message(Tab *tab, int line) {
    if (!tab || !tab->diagnostics) return NULL;

    // Return the worst (lowest severity number) diagnostic message for this line
    const char *msg = NULL;
    DiagnosticSeverity worst = 0;
    for (int i = 0; i < tab->diagnostic_count; i++) {
        if (tab->diagnostics[i].line == line) {
            if (worst == 0 || tab->diagnostics[i].severity < worst) {
                worst = tab->diagnostics[i].severity;
                msg = tab->diagnostics[i].message;
            }
        }
    }
    return msg;
}

// Semantic token functions

void clear_tab_tokens(Tab *tab) {
    if (!tab) return;
    free(tab->tokens);
    tab->tokens = NULL;
    tab->token_count = 0;
    tab->token_capacity = 0;
    free(tab->token_line_start);
    free(tab->token_line_count);
    tab->token_line_start = NULL;
    tab->token_line_count = NULL;
    tab->token_line_capacity = 0;
}

// ============== Code Folding Implementation ==============

void clear_tab_folds(Tab *tab) {
    if (!tab) return;
    free(tab->folds);
    tab->folds = NULL;
    tab->fold_count = 0;
    tab->fold_capacity = 0;
}

// get_fold_style_for_file is now editor_config_get_fold_style() in editor_config.c

void add_fold(Tab *tab, int start_line, int end_line) {
    if (!tab || start_line >= end_line) return;

    // Check if fold already exists
    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].start_line == start_line) return;
    }

    // Grow array if needed
    if (tab->fold_count >= tab->fold_capacity) {
        int new_capacity = tab->fold_capacity == 0 ? 16 : tab->fold_capacity * 2;
        Fold *new_folds = realloc(tab->folds, new_capacity * sizeof(Fold));
        if (!new_folds) return;
        tab->folds = new_folds;
        tab->fold_capacity = new_capacity;
    }

    tab->folds[tab->fold_count].start_line = start_line;
    tab->folds[tab->fold_count].end_line = end_line;
    tab->folds[tab->fold_count].is_folded = false;
    tab->fold_count++;
}

void detect_folds_braces(Tab *tab) {
    if (!tab || !tab->buffer) return;

    int line_count = tab->buffer->line_count;

    // Stack to track opening brace positions
    int *brace_stack = malloc(line_count * sizeof(int));
    int stack_top = -1;

    if (!brace_stack) return;

    for (int line = 0; line < line_count; line++) {
        const char *text = tab->buffer->lines[line];
        if (!text) continue;

        bool in_string = false;
        bool in_char = false;
        bool escape = false;

        for (int col = 0; text[col]; col++) {
            char c = text[col];

            if (escape) {
                escape = false;
                continue;
            }

            if (c == '\\') {
                escape = true;
                continue;
            }

            if (c == '"' && !in_char) {
                in_string = !in_string;
                continue;
            }

            if (c == '\'' && !in_string) {
                in_char = !in_char;
                continue;
            }

            if (in_string || in_char) continue;

            if (c == '{') {
                stack_top++;
                brace_stack[stack_top] = line;
            } else if (c == '}' && stack_top >= 0) {
                int start_line = brace_stack[stack_top];
                stack_top--;
                // Only create fold if it spans multiple lines
                if (line > start_line) {
                    add_fold(tab, start_line, line);
                }
            }
        }
    }

    free(brace_stack);
}

static int get_line_indent(const char *line) {
    if (!line) return 0;
    int indent = 0;
    while (*line == ' ' || *line == '\t') {
        if (*line == '\t') {
            indent += 4;  // Treat tab as 4 spaces
        } else {
            indent++;
        }
        line++;
    }
    // Empty or whitespace-only lines don't define indentation
    if (*line == '\0' || *line == '\n') return -1;
    return indent;
}

void detect_folds_indent(Tab *tab) {
    if (!tab || !tab->buffer) return;

    int line_count = tab->buffer->line_count;

    for (int line = 0; line < line_count; line++) {
        const char *text = tab->buffer->lines[line];
        if (!text) continue;

        // Check if line ends with ':'
        int len = strlen(text);
        bool ends_with_colon = false;
        for (int i = len - 1; i >= 0; i--) {
            if (text[i] == ':') {
                ends_with_colon = true;
                break;
            } else if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n') {
                break;
            }
        }

        if (!ends_with_colon) continue;

        // Find the indentation of this line
        int base_indent = get_line_indent(text);
        if (base_indent < 0) continue;

        // Find the end of the indented block
        int end_line = line;
        for (int j = line + 1; j < line_count; j++) {
            int indent = get_line_indent(tab->buffer->lines[j]);
            if (indent < 0) continue;  // Skip blank lines
            if (indent <= base_indent) {
                break;  // Block ended
            }
            end_line = j;
        }

        if (end_line > line) {
            add_fold(tab, line, end_line);
        }
    }
}

// Get heading level from a line (0 if not a heading)
static int get_heading_level(const char *line) {
    if (!line) return 0;

    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') line++;

    // Count # characters
    int level = 0;
    while (*line == '#') {
        level++;
        line++;
    }

    // Must be followed by space or end of line to be a valid heading
    if (level > 0 && (*line == ' ' || *line == '\t' || *line == '\0' || *line == '\n')) {
        return level;
    }

    return 0;
}

// Check if a line is a code fence (``` or ~~~)
static bool is_code_fence(const char *line) {
    if (!line) return false;

    // Skip leading whitespace (up to 3 spaces allowed)
    int spaces = 0;
    while (*line == ' ' && spaces < 3) {
        line++;
        spaces++;
    }

    // Check for ``` or ~~~
    if (strncmp(line, "```", 3) == 0 || strncmp(line, "~~~", 3) == 0) {
        return true;
    }

    return false;
}

void detect_folds_headings(Tab *tab) {
    if (!tab || !tab->buffer) return;

    int line_count = tab->buffer->line_count;
    bool in_code_block = false;

    for (int line = 0; line < line_count; line++) {
        const char *text = tab->buffer->lines[line];

        // Track code block state
        if (is_code_fence(text)) {
            in_code_block = !in_code_block;
            continue;
        }

        // Skip headings inside code blocks
        if (in_code_block) continue;

        int level = get_heading_level(text);

        if (level == 0) continue;  // Not a heading

        // Find the end of this heading's section
        // Section ends at the next heading of same or lower level (more important)
        // But we need to track code blocks in this inner loop too
        int end_line = line;
        bool inner_in_code = false;

        for (int j = line + 1; j < line_count; j++) {
            const char *inner_text = tab->buffer->lines[j];

            if (is_code_fence(inner_text)) {
                inner_in_code = !inner_in_code;
                end_line = j;
                continue;
            }

            if (inner_in_code) {
                end_line = j;
                continue;
            }

            int next_level = get_heading_level(inner_text);

            if (next_level > 0 && next_level <= level) {
                // Found a heading of same or higher importance - stop before it
                break;
            }
            end_line = j;
        }

        if (end_line > line) {
            add_fold(tab, line, end_line);
        }
    }
}

void detect_folds(Tab *tab) {
    if (!tab) return;

    // Clear existing folds (but preserve fold state for matching regions)
    // For simplicity, just clear for now
    clear_tab_folds(tab);

    tab->fold_style = editor_config_get_fold_style(tab->filename);

    switch (tab->fold_style) {
        case FOLD_STYLE_BRACES:
            detect_folds_braces(tab);
            break;
        case FOLD_STYLE_INDENT:
            detect_folds_indent(tab);
            break;
        case FOLD_STYLE_HEADINGS:
            detect_folds_headings(tab);
            break;
        case FOLD_STYLE_NONE:
        default:
            break;
    }
}

Fold *get_fold_at_line(Tab *tab, int line) {
    if (!tab) return NULL;
    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].start_line == line) {
            return &tab->folds[i];
        }
    }
    return NULL;
}

Fold *get_fold_containing_line(Tab *tab, int line) {
    if (!tab) return NULL;
    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].is_folded &&
            line > tab->folds[i].start_line &&
            line <= tab->folds[i].end_line) {
            return &tab->folds[i];
        }
    }
    return NULL;
}

bool is_line_visible(Tab *tab, int line) {
    return get_fold_containing_line(tab, line) == NULL;
}

int get_next_visible_line(Tab *tab, int line) {
    if (!tab) return line + 1;
    int next = line + 1;
    while (next < tab->buffer->line_count) {
        Fold *fold = get_fold_containing_line(tab, next);
        if (!fold) return next;
        next = fold->end_line + 1;
    }
    return next;
}

int get_prev_visible_line(Tab *tab, int line) {
    if (!tab) return line - 1;
    int prev = line - 1;
    while (prev >= 0) {
        Fold *fold = get_fold_containing_line(tab, prev);
        if (!fold) return prev;
        prev = fold->start_line - 1;
    }
    return prev;
}

void toggle_fold_at_line(Tab *tab, int line) {
    if (!tab) return;

    Fold *fold = get_fold_at_line(tab, line);
    if (fold) {
        fold->is_folded = !fold->is_folded;

        // If we just folded and cursor is inside the fold, move it to the fold line
        if (fold->is_folded &&
            tab->cursor_y > fold->start_line &&
            tab->cursor_y <= fold->end_line) {
            tab->cursor_y = fold->start_line;
            int line_len = tab->buffer->lines[tab->cursor_y] ?
                           strlen(tab->buffer->lines[tab->cursor_y]) : 0;
            if (tab->cursor_x > line_len) {
                tab->cursor_x = line_len;
            }
        }

        editor.needs_full_redraw = true;
    }
}

int count_folded_lines_before(Tab *tab, int line) {
    if (!tab) return 0;
    int count = 0;
    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].is_folded && tab->folds[i].end_line < line) {
            count += tab->folds[i].end_line - tab->folds[i].start_line;
        } else if (tab->folds[i].is_folded &&
                   tab->folds[i].start_line < line &&
                   tab->folds[i].end_line >= line) {
            // Partially before this line
            count += line - tab->folds[i].start_line - 1;
        }
    }
    return count;
}

int file_line_to_display_line(Tab *tab, int file_line) {
    if (!tab) return file_line;
    return file_line - count_folded_lines_before(tab, file_line);
}

int display_line_to_file_line(Tab *tab, int display_line) {
    if (!tab) return display_line;

    int file_line = 0;
    int current_display = 0;

    while (current_display < display_line && file_line < tab->buffer->line_count) {
        if (is_line_visible(tab, file_line)) {
            current_display++;
        }
        file_line++;
    }

    // Skip any invisible lines at this point
    while (file_line < tab->buffer->line_count && !is_line_visible(tab, file_line)) {
        file_line++;
    }

    return file_line;
}

// ============== End Code Folding Implementation ==============

void lsp_semantic_tokens_handler(const char *uri, SemanticToken *tokens, int count) {
    if (!uri) return;

    // Convert URI to path
    char *path = lsp_uri_to_path(uri);
    if (!path) return;

    // Find the tab with this file
    int tab_idx = find_tab_with_file(path);
    free(path);

    if (tab_idx < 0) return;

    Tab *tab = &editor.tabs[tab_idx];

    // Clear old tokens
    clear_tab_tokens(tab);

    if (count == 0 || !tokens) {
        editor.needs_full_redraw = true;
        return;
    }

    // Store new tokens
    tab->tokens = calloc(count, sizeof(StoredToken));
    if (!tab->tokens) return;

    tab->token_capacity = count;
    tab->token_count = count;

    for (int i = 0; i < count; i++) {
        tab->tokens[i].line = tokens[i].line;
        tab->tokens[i].col = tokens[i].col;
        tab->tokens[i].length = tokens[i].length;
        tab->tokens[i].type = tokens[i].type;
    }

    build_token_line_index(tab);

    editor.needs_full_redraw = true;
}

void request_semantic_tokens(Tab *tab) {
    if (!editor.lsp_enabled || !tab || !tab->filename || !tab->lsp_opened) return;
    lsp_request_semantic_tokens(tab->filename);
}

const char *get_token_color(SemanticTokenType type) {
    switch (type) {
        case TOKEN_KEYWORD:
        case TOKEN_MODIFIER:
            return FG_MAGENTA;
        case TOKEN_TYPE:
        case TOKEN_CLASS:
        case TOKEN_ENUM:
            return FG_YELLOW;
        case TOKEN_FUNCTION:
        case TOKEN_METHOD:
            return FG_BLUE;
        case TOKEN_VARIABLE:
        case TOKEN_PARAMETER:
        case TOKEN_PROPERTY:
            return FG_CYAN;
        case TOKEN_STRING:
            return FG_GREEN;
        case TOKEN_NUMBER:
            return FG_RED;
        case TOKEN_COMMENT:
            return FG_GREEN;
        case TOKEN_MACRO:
            return FG_MAGENTA;
        case TOKEN_NAMESPACE:
            return FG_YELLOW;
        case TOKEN_ENUM_MEMBER:
            return FG_CYAN;
        case TOKEN_OPERATOR:
        case TOKEN_UNKNOWN:
        default:
            return NULL;  // Default color
    }
}

const char* get_file_size_str(long size, bool is_dir) {
    static char size_str[32];
    if (is_dir) {
        return "<DIR>";
    }
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
            free(editor.file_list[i].name);
        }
        free(editor.file_list);
        editor.file_list = NULL;
    }
    editor.file_count = 0;
    editor.file_capacity = 0;
}

int file_list_compare(const void *a, const void *b) {
    const FileEntry *entry_a = (const FileEntry *)a;
    const FileEntry *entry_b = (const FileEntry *)b;
    const char *name_a = entry_a->name;
    const char *name_b = entry_b->name;

    // ".." always comes first
    if (strcmp(name_a, "..") == 0) return -1;
    if (strcmp(name_b, "..") == 0) return 1;

    // Directories come before files
    if (entry_a->is_dir && !entry_b->is_dir) return -1;
    if (!entry_a->is_dir && entry_b->is_dir) return 1;

    // Within same type, sort alphabetically (case-insensitive)
    return strcasecmp(name_a, name_b);
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
    editor.file_list = malloc(editor.file_capacity * sizeof(FileEntry));
    if (!editor.file_list) {
        closedir(dir);
        return;
    }

    // Second pass: store entries
    editor.file_count = 0;
    while ((entry = readdir(dir)) != NULL && editor.file_count < editor.file_capacity) {
        if (strcmp(entry->d_name, ".") == 0) continue; // Skip current dir
        FileEntry *file_entry = &editor.file_list[editor.file_count];
        file_entry->name = strdup(entry->d_name);
        file_entry->is_dir = false;
        file_entry->size = 0;

        if (file_entry->name) {
            if (strcmp(file_entry->name, "..") == 0) {
                file_entry->is_dir = true;
            } else {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", editor.current_directory, file_entry->name);
                struct stat statbuf;
                if (stat(path, &statbuf) == 0) {
                    file_entry->is_dir = S_ISDIR(statbuf.st_mode);
                    file_entry->size = statbuf.st_size;
                }
            }
            editor.file_count++;
        }
    }

    closedir(dir);

    // Sort: ".." first, then directories, then files (alphabetically within each group)
    if (editor.file_count > 1) {
        qsort(editor.file_list, editor.file_count, sizeof(FileEntry), file_list_compare);
    }

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
    
    FileEntry *selected_entry = &editor.file_list[editor.file_manager_cursor];
    if (!selected_entry || !selected_entry->name) return;
    char *selected = selected_entry->name;
    
    // Build full path
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", editor.current_directory, selected);
    
    if (selected_entry->is_dir || is_directory(full_path)) {
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
        // Check if file is already open in a tab
        int existing_tab = find_tab_with_file(full_path);
        if (existing_tab >= 0) {
            // File is already open, switch to that tab
            switch_to_tab(existing_tab);
            set_status_message("Switched to existing tab %d (%s)", existing_tab + 1, selected);
            editor.file_manager_focused = false; // Return focus to editor
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
    }
    
    editor.needs_full_redraw = true;
}

void draw_file_manager(RenderBuf *rb) {
    if (!editor.file_manager_visible) return;
    
    int start_col = 1;
    int width = editor.file_manager_width;
    int visible_height = editor.screen_rows - 2; // Account for tab bar and status line
    
    // Draw file manager background and border
    for (int y = 0; y < visible_height; y++) {
        render_move_cursor(rb, y + 2, start_col); // +2 for tab bar
        
        if (editor.file_manager_focused) {
            render_buf_append(rb, "\033[44m"); // Blue background when focused
        } else {
            render_buf_append(rb, "\033[100m"); // Dark gray background when not focused
        }
        
        // Clear the line
        for (int x = 0; x < width; x++) {
            render_buf_append(rb, " ");
        }
        
        // Draw file entry if available
        int file_index = y + editor.file_manager_offset;
        if (file_index < editor.file_count) {
            render_move_cursor(rb, y + 2, start_col);
            FileEntry *entry = &editor.file_list[file_index];
            if (entry->name) {
                char *filename = entry->name;
            
            // Highlight current selection
            if (file_index == editor.file_manager_cursor) {
                render_buf_append(rb, "\033[47m\033[30m"); // White background, black text
            }
            
            // Truncate filename if too long
            int max_name_len = width - 8; // Leave space for size
            if ((int)strlen(filename) > max_name_len) {
                render_buf_appendf(rb, "> %-*.*s", max_name_len - 2, max_name_len - 2, filename);
            } else {
                render_buf_appendf(rb, "> %-*s", max_name_len, filename);
            }
            
            // Show size or <DIR>
                render_buf_appendf(rb, " %6s", get_file_size_str(entry->size, entry->is_dir));
            }
        }
        
        render_buf_append(rb, "\033[0m"); // Reset formatting
    }
    
    // Draw vertical border on the right
    if (!editor.file_manager_overlay_mode) {
        for (int y = 0; y < visible_height; y++) {
            render_move_cursor(rb, y + 2, start_col + width);
            render_buf_append(rb, "\033[37m|\033[0m"); // Gray vertical line
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

    // Load LSP configuration
    editor_config_load();

    // LSP will be initialized lazily when opening a supported file
    editor.lsp_enabled = false;

    // Create first tab
    const char* filename = (argc > 1) ? argv[1] : NULL;
    if (create_new_tab(filename) < 0) {
        terminal_cleanup();
        fprintf(stderr, "Failed to create initial tab\n");
        return 1;
    }
    
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    terminal_enable_mouse();
    
    editor.line_number_width = 8;  // 1 fold indicator + 6 digits + 1 space
    editor.needs_full_redraw = true;
    
    Tab* tab = get_current_tab();
    if (tab && tab->filename) {
        set_status_message("Loaded file: %s", tab->filename);
        // Notify LSP about the initially opened file
        notify_lsp_file_opened(tab);
    } else {
        set_status_message("Ctrl+E:file manager, Ctrl+T:new tab, Ctrl+O:open file, Ctrl+W:close, Ctrl+[/]:switch tabs, Ctrl+S:save, Ctrl+Q:quit");
    }
    
    struct timespec last_frame = {0};
    clock_gettime(CLOCK_MONOTONIC, &last_frame);
    bool pending_draw = true;

    while (1) {
        // Check for window resize
        int current_rows, current_cols;
        current_rows = current_cols = 0;
        terminal_get_window_size(&current_rows, &current_cols);
        if (current_rows > 0 && current_cols > 0 && 
            (current_rows != editor.screen_rows || current_cols != editor.screen_cols)) {
            editor.resize_pending = true;
            pending_draw = true;
        }
        
        process_resize();
        scroll_if_needed();
        
        // Check for external file changes (only if no dialog is active)
        if (!editor.quit_confirmation_active && !editor.reload_confirmation_active) {
            check_file_changes();
        }
        
        // Use select to check if input is available with timeout
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        // Also monitor LSP stdout for incoming messages
        int lsp_fd = lsp_get_fd();
        int max_fd = STDIN_FILENO;
        if (lsp_fd >= 0) {
            FD_SET(lsp_fd, &readfds);
            if (lsp_fd > max_fd) max_fd = lsp_fd;
        }

        long remaining_ms = frame_remaining_ms(&last_frame, 16);
        timeout.tv_sec = remaining_ms / 1000;
        timeout.tv_usec = (remaining_ms % 1000) * 1000;

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        // Process LSP messages if available
        if (activity > 0 && lsp_fd >= 0 && FD_ISSET(lsp_fd, &readfds)) {
            lsp_process_incoming();
            pending_draw = true;
        }

        int c = 0;
        if (activity > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            // Input is available, read it
            c = terminal_read_key();
            pending_draw = true;
        } else {
            // No user input available, continue loop for resize checking
            if (frame_due(&last_frame, 16) && pending_draw) {
                draw_screen();
                pending_draw = false;
            }
            continue;
        }
        
        // Remove debug key codes - status bar now shows file info
        
        // Handle quit confirmation dialog first (highest priority when active)
        if (editor.quit_confirmation_active) {
            if (c == 'q' || c == 'Q') {
                // User confirmed quit
                break;
            } else {
                // User cancelled quit
                editor.quit_confirmation_active = false;
                editor.needs_full_redraw = true;
                pending_draw = true;
            }
        
        // Handle reload confirmation dialog (second highest priority when active)
        } else if (editor.reload_confirmation_active) {
            if (c == 'r' || c == 'R') {
                // User wants to reload
                reload_file_in_tab(editor.reload_tab_index);
                editor.reload_confirmation_active = false;
                pending_draw = true;
            } else {
                // User wants to keep current version, just update the mtime to stop asking
                Tab* tab = &editor.tabs[editor.reload_tab_index];
                if (tab && tab->filename) {
                    tab->file_mtime = get_file_mtime(tab->filename);
                }
                editor.reload_confirmation_active = false;
                editor.needs_full_redraw = true;
                set_status_message("Keeping current version");
                pending_draw = true;
            }
        
        // Handle file manager input first (highest priority when focused)
        } else if (editor.file_manager_visible && editor.file_manager_focused) {
            if (c == 27) {  // Escape key
                editor.file_manager_focused = false;
                // Focus change doesn't need full screen redraw
                pending_draw = true;
            } else if (c == CTRL_KEY('q')) {
                // Allow Ctrl+Q to quit even when file manager is focused
                if (has_unsaved_changes()) {
                    show_quit_confirmation();
                } else {
                    break;
                }
            } else if (c == CTRL_KEY('e')) {
                // Ctrl+E when file manager focused -> hide it
                editor.file_manager_focused = false;
                toggle_file_manager();
                pending_draw = true;
            } else if (c == '\r' || c == '\n') {  // Enter key
                file_manager_select_item();
                pending_draw = true;
            } else if (c == ARROW_UP) {
                file_manager_navigate(-1);
                pending_draw = true;
            } else if (c == ARROW_DOWN) {
                file_manager_navigate(1);
                pending_draw = true;
            } else if (c == '\t') {  // Tab key - return focus to editor
                editor.file_manager_focused = false;
                set_status_message("Focus: Editor");
                pending_draw = true;
            }
            // Don't process any other keys when file manager is focused
            // This prevents text input from affecting the editor
        } else if (editor.filename_input_mode) {
            if (c == 27) {  // Escape key
                exit_filename_input_mode();
                pending_draw = true;
            } else if (c == '\r' || c == '\n') {  // Enter key
                process_filename_input();
                pending_draw = true;
            } else if (c == 127 || c == CTRL_KEY('h')) {  // Backspace
                if (editor.filename_input_len > 0) {
                    editor.filename_input_len--;
                    editor.filename_input[editor.filename_input_len] = '\0';
                }
                pending_draw = true;
            } else if (c >= 32 && c < 127) {  // Printable characters
                if (editor.filename_input_len < editor.filename_input_capacity - 1) {
                    editor.filename_input[editor.filename_input_len] = c;
                    editor.filename_input_len++;
                    editor.filename_input[editor.filename_input_len] = '\0';
                }
                pending_draw = true;
            }
        } else if (editor.find_mode) {
            if (c == 27) {  // Escape key
                exit_find_mode();
                pending_draw = true;
            } else if (c == CTRL_KEY('n')) {
                find_next();
                pending_draw = true;
            } else if (c == CTRL_KEY('p')) {
                find_previous();
                pending_draw = true;
            } else if (c == 127 || c == CTRL_KEY('h')) {  // Backspace
                if (editor.search_query_len > 0) {
                    editor.search_query_len--;
                    editor.search_query[editor.search_query_len] = '\0';
                    find_matches();
                    if (editor.total_matches > 0) {
                        jump_to_match(editor.current_match);
                    }
                }
                pending_draw = true;
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
                pending_draw = true;
            }
        } else if (c == '\t') {  // Tab key - insert tab character
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            }
            insert_char('\t');
            pending_draw = true;
        } else if (c == CTRL_KEY('e')) {
            // Smart file manager toggle:
            // - Hidden -> show and focus
            // - Visible + editor focused -> focus file manager
            // - Visible + file manager focused -> hide
            if (!editor.file_manager_visible) {
                toggle_file_manager();
                editor.file_manager_focused = true;
            } else if (!editor.file_manager_focused) {
                editor.file_manager_focused = true;
                set_status_message("Focus: File Manager");
            } else {
                editor.file_manager_focused = false;
                toggle_file_manager();
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('f')) {
            enter_find_mode();
            pending_draw = true;
        } else if (c == CTRL_KEY('t')) {
            // Create new tab
            int new_tab = create_new_tab(NULL);
            if (new_tab >= 0) {
                switch_to_tab(new_tab);
                set_status_message("Created new tab %d", new_tab + 1);
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('w')) {
            // Close current tab
            if (editor.tab_count > 1) {
                close_tab(editor.current_tab);
                set_status_message("Closed tab");
            } else {
                set_status_message("Cannot close last tab");
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('o')) {
            // Open file in new tab
            enter_filename_input_mode();
            pending_draw = true;
        } else if (c == CTRL_KEY('[') || c == CTRL_SHIFT_TAB) {
            // Ctrl+[ or Ctrl+Shift+Tab - Previous tab
            switch_to_prev_tab();
            pending_draw = true;
        } else if (c == CTRL_KEY(']') || c == CTRL_TAB) {
            // Ctrl+] or Ctrl+Tab - Next tab
            switch_to_next_tab();
            pending_draw = true;
        } else if (c == CTRL_KEY('q')) {
            if (has_unsaved_changes()) {
                show_quit_confirmation();
            } else {
                break;
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('s')) {
            save_file();
            pending_draw = true;
        } else if (c == CTRL_KEY('c')) {
            char *selected = get_selected_text();
            if (selected) {
                clipboard_set(selected);
                free(selected);
                set_status_message("Copied to clipboard");
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('x')) {
            char *selected = get_selected_text();
            if (selected) {
                clipboard_set(selected);
                free(selected);
                delete_selection();
                set_status_message("Cut to clipboard");
            }
            pending_draw = true;
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
            pending_draw = true;
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
            pending_draw = true;
        } else if (c == '\r' || c == '\n') {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            }
            insert_newline();
            pending_draw = true;
        } else if (c == 127 || c == CTRL_KEY('h')) {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            } else {
                delete_char();
            }
        } else if (c == F2_KEY) {
            // Toggle fold at cursor line
            Tab *tab = get_current_tab();
            if (tab) {
                Fold *fold = get_fold_at_line(tab, tab->cursor_y);
                if (fold) {
                    toggle_fold_at_line(tab, tab->cursor_y);
                    set_status_message(fold->is_folded ? "Folded %d lines" : "Unfolded",
                                       fold->end_line - fold->start_line);
                }
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
        } else if (c == MOUSE_SCROLL_UP) {
            Tab* tab = get_current_tab();
            if (tab) {
                int scroll_lines = 3;
                tab->offset_y -= scroll_lines;
                if (tab->offset_y < 0) tab->offset_y = 0;
                // Keep cursor visible
                if (tab->cursor_y >= tab->offset_y + editor.screen_rows - 2) {
                    tab->cursor_y = tab->offset_y + editor.screen_rows - 3;
                }
                editor.needs_full_redraw = true;
            }
        } else if (c == MOUSE_SCROLL_DOWN) {
            Tab* tab = get_current_tab();
            if (tab) {
                int scroll_lines = 3;
                int max_offset = tab->buffer->line_count - (editor.screen_rows - 2);
                if (max_offset < 0) max_offset = 0;
                tab->offset_y += scroll_lines;
                if (tab->offset_y > max_offset) tab->offset_y = max_offset;
                // Keep cursor visible
                if (tab->cursor_y < tab->offset_y) {
                    tab->cursor_y = tab->offset_y;
                }
                editor.needs_full_redraw = true;
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

        if (frame_due(&last_frame, 16) && pending_draw) {
            draw_screen();
            pending_draw = false;
        }
    }
    
    cleanup_and_exit(0);
    return 0;
}
