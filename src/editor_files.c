#define _GNU_SOURCE
#include "editor_files.h"
#include "buffer.h"
#include "editor.h"
#include "editor_tabs.h"
#include "editor_selection.h"
#include "lsp_integration.h"
#include "render.h"
#include "terminal.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void enter_filename_input_mode(void) {
    editor.filename_input_mode = true;
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
    if (!editor.filename_input || editor.filename_input_len == 0) {
        exit_filename_input_mode();
        return;
    }
    
    // Check if file is already open in another tab
    int existing_tab = find_tab_with_file(editor.filename_input);
    if (existing_tab >= 0) {
        switch_to_tab(existing_tab);
        set_status_message("Switched to existing tab %d (%s)", existing_tab + 1, editor.filename_input);
    } else {
        // Open new tab
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

int get_file_size(void) {
    Tab* tab = get_current_tab();
    if (!tab || !tab->filename) return 0;

    struct stat st;
    if (stat(tab->filename, &st) != 0) return 0;
    return (int)st.st_size;
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

void save_file(void) {
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

void delete_char(void) {
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

void insert_newline(void) {
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

time_t get_file_mtime(const char* filename) {
    struct stat statbuf;
    if (stat(filename, &statbuf) != 0) {
        return 0;
    }
    return statbuf.st_mtime;
}

void check_file_changes(void) {
    for (int i = 0; i < editor.tab_count; i++) {
        Tab* tab = &editor.tabs[i];
        if (!tab->filename) continue;
        
        time_t current_mtime = get_file_mtime(tab->filename);
        if (current_mtime > tab->file_mtime && !tab->modified) {
            show_reload_confirmation(i);
            return;
        } else if (current_mtime > tab->file_mtime && tab->modified) {
            show_reload_confirmation(i);
            return;
        }
    }
}

void show_quit_confirmation(void) {
    editor.quit_confirmation_active = true;
    editor.needs_full_redraw = true;
}

void show_reload_confirmation(int tab_index) {
    editor.reload_confirmation_active = true;
    editor.reload_tab_index = tab_index;
    editor.needs_full_redraw = true;
}

void reload_file_in_tab(int tab_index) {
    if (tab_index < 0 || tab_index >= editor.tab_count) return;
    
    Tab* tab = &editor.tabs[tab_index];
    if (!tab->filename) return;
    
    // Reload the file from disk
    buffer_free(tab->buffer);
    tab->buffer = buffer_create();
    if (!tab->buffer) return;
    
    if (buffer_load_from_file(tab->buffer, tab->filename)) {
        tab->modified = false;
        tab->file_mtime = get_file_mtime(tab->filename);
        set_status_message("File reloaded: %s", tab->filename);
        editor.needs_full_redraw = true;
    } else {
        set_status_message("Error: Could not reload file %s", tab->filename);
    }
}
