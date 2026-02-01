#define _GNU_SOURCE
#include "editor_app.h"
#include "clipboard.h"
#include "editor.h"
#include "editor_cursor.h"
#include "editor_files.h"
#include "editor_hover.h"
#include "editor_folds.h"
#include "editor_mouse.h"
#include "editor_search.h"
#include "editor_selection.h"
#include "editor_tabs.h"
#include "file_manager.h"
#include "lsp.h"
#include "lsp_integration.h"
#include "render.h"
#include "terminal.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

Editor editor = {0};

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

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000LL);
}
void cleanup_and_exit(int status) {
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
    if (editor.hover_text) free(editor.hover_text);
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
    editor.resize_pending = true;
}

void process_resize(void) {
    if (!editor.resize_pending) return;
    
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    editor.needs_full_redraw = true;
    editor.resize_pending = false;
}

int editor_run(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (!terminal_init()) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return 1;
    }
    
    editor.tabs = NULL;
    editor.tab_count = 0;
    editor.tab_capacity = 0;
    editor.current_tab = 0;
    
    editor.file_manager_visible = false;
    editor.file_manager_overlay_mode = false;
    editor.file_manager_width = 25;
    editor.current_directory = NULL;
    editor.file_list = NULL;
    editor.file_count = 0;
    editor.file_capacity = 0;
    editor.file_manager_cursor = 0;
    editor.file_manager_offset = 0;
    editor.file_manager_focused = false;

    editor_config_load();

    editor.lsp_enabled = false;

    const char* filename = (argc > 1) ? argv[1] : NULL;
    if (create_new_tab(filename) < 0) {
        terminal_cleanup();
        fprintf(stderr, "Failed to create initial tab\n");
        return 1;
    }
    
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    terminal_enable_mouse();
    
    editor.line_number_width = 8;
    editor.needs_full_redraw = true;
    
    Tab* tab = get_current_tab();
    if (tab && tab->filename) {
        set_status_message("Loaded file: %s", tab->filename);
        notify_lsp_file_opened(tab);
    } else {
        set_status_message("Ctrl+E:file manager, Ctrl+T:new tab, Ctrl+O:open file, Ctrl+W:close, Ctrl+[/]:switch tabs, Ctrl+S:save, Ctrl+Q:quit");
    }
    
    struct timespec last_frame = {0};
    clock_gettime(CLOCK_MONOTONIC, &last_frame);
    bool pending_draw = true;

    while (1) {
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
        hover_process_requests();
        process_semantic_tokens_requests();
        if (editor.hover_request_active &&
            (monotonic_ms() - editor.hover_request_ms > 1000)) {
            editor.hover_request_active = false;
            hover_clear();
            set_status_message("Hover: no response");
        }
        
        if (!editor.quit_confirmation_active && !editor.reload_confirmation_active) {
            check_file_changes();
        }
        
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

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

        if (activity > 0 && lsp_fd >= 0 && FD_ISSET(lsp_fd, &readfds)) {
            lsp_process_incoming();
            pending_draw = true;
        }

        int c = 0;
        if (activity > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            c = terminal_read_key();
            pending_draw = true;
            if (c != 0) {
                hover_clear();
            }
        } else {
            if (frame_due(&last_frame, 16) && pending_draw) {
                draw_screen();
                pending_draw = false;
            }
            continue;
        }
        
        if (editor.quit_confirmation_active) {
            if (c == 'q' || c == 'Q') {
                break;
            } else {
                editor.quit_confirmation_active = false;
                editor.needs_full_redraw = true;
                pending_draw = true;
            }
        
        } else if (editor.reload_confirmation_active) {
            if (c == 'r' || c == 'R') {
                reload_file_in_tab(editor.reload_tab_index);
                editor.reload_confirmation_active = false;
                pending_draw = true;
            } else {
                Tab* tab = &editor.tabs[editor.reload_tab_index];
                if (tab && tab->filename) {
                    tab->file_mtime = get_file_mtime(tab->filename);
                }
                editor.reload_confirmation_active = false;
                editor.needs_full_redraw = true;
                set_status_message("Keeping current version");
                pending_draw = true;
            }
        
        } else if (editor.file_manager_visible && editor.file_manager_focused) {
            if (c == 27) {
                editor.file_manager_focused = false;
                pending_draw = true;
            } else if (c == CTRL_KEY('q')) {
                if (has_unsaved_changes()) {
                    show_quit_confirmation();
                } else {
                    break;
                }
            } else if (c == CTRL_KEY('e')) {
                editor.file_manager_focused = false;
                toggle_file_manager();
                pending_draw = true;
            } else if (c == '\r' || c == '\n') {
                file_manager_select_item();
                pending_draw = true;
            } else if (c == ARROW_UP) {
                file_manager_navigate(-1);
                pending_draw = true;
            } else if (c == ARROW_DOWN) {
                file_manager_navigate(1);
                pending_draw = true;
            } else if (c == '\t') {
                editor.file_manager_focused = false;
                set_status_message("Focus: Editor");
                pending_draw = true;
            }
        } else if (editor.filename_input_mode) {
            if (c == 27) {
                exit_filename_input_mode();
                pending_draw = true;
            } else if (c == '\r' || c == '\n') {
                process_filename_input();
                pending_draw = true;
            } else if (c == 127 || c == CTRL_KEY('h')) {
                if (editor.filename_input_len > 0) {
                    editor.filename_input_len--;
                    editor.filename_input[editor.filename_input_len] = '\0';
                }
                pending_draw = true;
            } else if (c >= 32 && c < 127) {
                if (editor.filename_input_len < editor.filename_input_capacity - 1) {
                    editor.filename_input[editor.filename_input_len] = c;
                    editor.filename_input_len++;
                    editor.filename_input[editor.filename_input_len] = '\0';
                }
                pending_draw = true;
            }
        } else if (editor.find_mode) {
            if (c == 27) {
                exit_find_mode();
                pending_draw = true;
            } else if (c == CTRL_KEY('n')) {
                find_next();
                pending_draw = true;
            } else if (c == CTRL_KEY('p')) {
                find_previous();
                pending_draw = true;
            } else if (c == 127 || c == CTRL_KEY('h')) {
                if (editor.search_query_len > 0) {
                    editor.search_query_len--;
                    editor.search_query[editor.search_query_len] = '\0';
                    find_matches();
                    if (editor.total_matches > 0) {
                        jump_to_match(editor.current_match);
                    }
                }
                pending_draw = true;
            } else if (c >= 32 && c < 127) {
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
        } else if (c == '\t') {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            }
            insert_char('\t');
            pending_draw = true;
        } else if (c == CTRL_KEY('e')) {
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
            int new_tab = create_new_tab(NULL);
            if (new_tab >= 0) {
                switch_to_tab(new_tab);
                set_status_message("Created new tab %d", new_tab + 1);
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('w')) {
            if (editor.tab_count > 1) {
                close_tab(editor.current_tab);
                set_status_message("Closed tab");
            } else {
                set_status_message("Cannot close last tab");
            }
            pending_draw = true;
        } else if (c == CTRL_KEY('o')) {
            enter_filename_input_mode();
            pending_draw = true;
        } else if (c == CTRL_KEY('[') || c == CTRL_SHIFT_TAB) {
            switch_to_prev_tab();
            pending_draw = true;
        } else if (c == CTRL_KEY(']') || c == CTRL_TAB) {
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
        } else if (c == CTRL_KEY('g')) {
            Tab* tab = get_current_tab();
            if (!tab || !tab->filename) {
                set_status_message("Hover: no file");
            } else if (!editor.lsp_enabled || !tab->lsp_opened) {
                set_status_message("Hover: LSP not active");
            } else if (!lsp_hover_is_supported()) {
                set_status_message("Hover: not supported by LSP");
            } else {
                if (editor.hover_active) {
                    hover_clear();
                } else {
                    hover_request_cursor(tab);
                }
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
                int end_x = tab->select_end_x;
                int end_y = tab->select_end_y;
                int start_x = tab->select_start_x;
                int start_y = tab->select_start_y;
                
                if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
                    int temp_x = start_x, temp_y = start_y;
                    start_x = end_x; start_y = end_y;
                    end_x = temp_x; end_y = temp_y;
                }
                
                if (tab->cursor_x == start_x && tab->cursor_y == start_y) {
                    clear_selection();
                } else {
                    clear_selection();
                    move_cursor(-1, 0);
                }
            } else {
                move_cursor(-1, 0);
            }
        } else if (c == ARROW_RIGHT) {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                int end_x = tab->select_end_x;
                int end_y = tab->select_end_y;
                int start_x = tab->select_start_x;
                int start_y = tab->select_start_y;
                
                if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
                    int temp_x = start_x, temp_y = start_y;
                    start_x = end_x; start_y = end_y;
                    end_x = temp_x; end_y = temp_y;
                }
                
                if (tab->cursor_x == end_x && tab->cursor_y == end_y) {
                    clear_selection();
                } else {
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
