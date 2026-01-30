#define _GNU_SOURCE
#include "file_manager.h"
#include "editor.h"
#include "terminal.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int file_list_compare(const void *a, const void *b) {
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
