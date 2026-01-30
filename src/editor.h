#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>
#include <time.h>

#include "buffer.h"
#include "editor_config.h"
#include "lsp.h"

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

extern Editor editor;

// Shared helpers
Tab* get_current_tab(void);
DiagnosticSeverity get_line_diagnostic_severity(Tab *tab, int line);
const char *get_line_diagnostic_message(Tab *tab, int line);
const char *get_token_color(SemanticTokenType type);
Fold *get_fold_at_line(Tab *tab, int line);
Fold *get_fold_containing_line(Tab *tab, int line);
bool is_line_visible(Tab *tab, int line);
int get_next_visible_line(Tab *tab, int line);
int get_prev_visible_line(Tab *tab, int line);

// Cross-module editor helpers
void set_status_message(const char *fmt, ...);
int create_new_tab(const char* filename);
void switch_to_tab(int tab_index);
int find_tab_with_file(const char* filename);
bool is_directory(const char* filepath);
const char* get_file_size_str(long size, bool is_dir);
int get_file_size(void);
const char* format_file_size(int bytes);

#endif
