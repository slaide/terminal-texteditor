#define _GNU_SOURCE
#include "editor_tabs.h"
#include "buffer.h"
#include "editor_config.h"
#include "lsp_integration.h"
#include "editor_folds.h"
#include "editor_selection.h"
#include "editor_files.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Tab* get_current_tab(void) {
    if (editor.current_tab < 0 || editor.current_tab >= editor.tab_count) return NULL;
    return &editor.tabs[editor.current_tab];
}

int create_new_tab(const char* filename) {
    if (editor.tab_count >= editor.tab_capacity) {
        int new_capacity = editor.tab_capacity == 0 ? 4 : editor.tab_capacity * 2;
        Tab *new_tabs = realloc(editor.tabs, new_capacity * sizeof(Tab));
        if (!new_tabs) return -1;
        editor.tabs = new_tabs;
        editor.tab_capacity = new_capacity;
    }
    
    Tab *tab = &editor.tabs[editor.tab_count];
    memset(tab, 0, sizeof(Tab));
    
    tab->buffer = buffer_create();
    if (!tab->buffer) return -1;
    
    if (filename) {
        tab->filename = strdup(filename);
        if (!tab->filename) {
            buffer_free(tab->buffer);
            return -1;
        }
        // Load file content
        if (!buffer_load_from_file(tab->buffer, filename)) {
            free(tab->filename);
            tab->filename = NULL;
        }
        tab->file_mtime = get_file_mtime(filename);
    }
    
    tab->cursor_x = 0;
    tab->cursor_y = 0;
    tab->offset_x = 0;
    tab->offset_y = 0;
    tab->modified = false;
    tab->selecting = false;
    tab->lsp_opened = false;
    tab->lsp_version = 1;
    tab->lsp_name = NULL;
    tab->tokens = NULL;
    tab->token_count = 0;
    tab->token_capacity = 0;
    tab->token_line_start = NULL;
    tab->token_line_count = NULL;
    tab->token_line_capacity = 0;
    tab->tokens_pending = false;
    tab->tokens_last_change_ms = 0;
    tab->diagnostics = NULL;
    tab->diagnostic_count = 0;
    tab->diagnostic_capacity = 0;
    tab->folds = NULL;
    tab->fold_count = 0;
    tab->fold_capacity = 0;
    tab->fold_style = editor_config_get_fold_style(filename);
    
    detect_folds(tab);
    
    editor.tab_count++;
    return editor.tab_count - 1;
}

void free_tab(Tab* tab) {
    if (!tab) return;
    
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
