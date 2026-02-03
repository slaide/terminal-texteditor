#define _GNU_SOURCE
#include "render.h"
#include "editor.h"
#include "file_manager.h"
#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

static void draw_modal(RenderBuf *rb, const char* title, const char* message, const char* bg_color, const char* fg_color);
static void draw_completion_popup(RenderBuf *rb);
static void draw_hover_popup(RenderBuf *rb);

void render_buf_init(RenderBuf *rb) {
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

void render_buf_free(RenderBuf *rb) {
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

bool render_buf_ensure(RenderBuf *rb, int extra) {
    if (rb->len + extra + 1 <= rb->cap) return true;
    int new_cap = rb->cap == 0 ? 256 : rb->cap * 2;
    while (new_cap < rb->len + extra + 1) new_cap *= 2;
    char *new_data = realloc(rb->data, new_cap);
    if (!new_data) return false;
    rb->data = new_data;
    rb->cap = new_cap;
    return true;
}

void render_buf_append(RenderBuf *rb, const char *s) {
    if (!s) return;
    int n = strlen(s);
    if (!render_buf_ensure(rb, n)) return;
    memcpy(rb->data + rb->len, s, n);
    rb->len += n;
    rb->data[rb->len] = '\0';
}

void render_buf_append_char(RenderBuf *rb, char c) {
    if (!render_buf_ensure(rb, 1)) return;
    rb->data[rb->len++] = c;
    rb->data[rb->len] = '\0';
}

void render_buf_appendf(RenderBuf *rb, const char *fmt, ...) {
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

void render_move_cursor(RenderBuf *rb, int row, int col) {
    render_buf_appendf(rb, "\033[%d;%dH", row, col);
}

void render_clear_screen(RenderBuf *rb) {
    render_buf_append(rb, "\033[2J\033[H");
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

static void draw_line_to_buf(RenderBuf *rb, int screen_y, int file_y, int start_col) {
    Tab* tab = get_current_tab();
    if (!tab || !rb) return;

    render_move_cursor(rb, screen_y + 2, start_col);
    render_buf_append(rb, "\033[K");

    int available_cols = editor.screen_cols - start_col + 1;
    if (available_cols < 1) available_cols = 1;

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
            int display_len = available_cols - editor.line_number_width - 20;
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
            int display_len = available_cols - editor.line_number_width;
            if (display_len < 0) display_len = 0;

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
                    }
                    if (in_selection) render_buf_append(rb, "\033[7m"); // Keep selection after color changes
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

static void draw_status_line(RenderBuf *rb) {
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

void draw_quit_confirmation(RenderBuf *rb) {
    if (!editor.quit_confirmation_active) return;
    
    draw_modal(rb, "You have unsaved changes!", 
               "Press 'q' to quit anyway, or any other key to cancel",
               STYLE_QUIT_DIALOG, 
               FG_WHITE);
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

static int completion_line_len(const CompletionEntry *item) {
    if (!item || !item->label) return 0;
    int len = (int)strlen(item->label);
    if (item->detail && item->detail[0] != '\0') {
        len += 3 + (int)strlen(item->detail); // " : "
    }
    if (item->doc && item->doc[0] != '\0') {
        len += 3 + (int)strlen(item->doc); // " - "
    }
    return len;
}

static void render_completion_line(RenderBuf *rb, const CompletionEntry *item, int width) {
    if (!item || !item->label || width <= 0) return;
    int remaining = width;

    int label_len = (int)strlen(item->label);
    int take = label_len < remaining ? label_len : remaining;
    render_buf_appendf(rb, "%.*s", take, item->label);
    remaining -= take;
    if (remaining <= 0) return;

    if (item->detail && item->detail[0] != '\0' && remaining > 0) {
        const char *sep = " : ";
        int sep_len = 3;
        if (sep_len > remaining) return;
        render_buf_append(rb, sep);
        remaining -= sep_len;
        if (remaining <= 0) return;
        int detail_len = (int)strlen(item->detail);
        take = detail_len < remaining ? detail_len : remaining;
        render_buf_appendf(rb, "%.*s", take, item->detail);
        remaining -= take;
    }

    if (item->doc && item->doc[0] != '\0' && remaining > 0) {
        const char *sep = " - ";
        int sep_len = 3;
        if (sep_len > remaining) return;
        render_buf_append(rb, sep);
        remaining -= sep_len;
        if (remaining <= 0) return;
        int doc_len = (int)strlen(item->doc);
        take = doc_len < remaining ? doc_len : remaining;
        render_buf_appendf(rb, "%.*s", take, item->doc);
    }
}

static void draw_completion_popup(RenderBuf *rb) {
    if (!editor.completion_active || editor.completion_count <= 0) return;
    if (editor.quit_confirmation_active || editor.reload_confirmation_active) return;

    int max_width = editor.screen_cols - 4;
    if (max_width < 20) return;

    int max_line_len = 0;
    for (int i = 0; i < editor.completion_count; i++) {
        int line_len = completion_line_len(&editor.completion_items[i]);
        if (line_len > max_line_len) max_line_len = line_len;
    }

    int content_width = max_line_len;
    if (content_width < 20) content_width = 20;
    if (content_width > max_width - 2) content_width = max_width - 2;

    int max_height = editor.screen_rows - 3;
    if (max_height < 3) return;

    bool show_no_match = editor.completion_prefix &&
                         editor.completion_prefix[0] != '\0' &&
                         !editor.completion_prefix_match;
    int total_lines = editor.completion_count + (show_no_match ? 1 : 0);
    int visible_lines = total_lines;
    if (visible_lines > max_height - 2) visible_lines = max_height - 2;
    if (visible_lines < 1) visible_lines = 1;

    int popup_width = content_width + 2;
    int popup_height = visible_lines + 2;

    int start_col = editor.completion_screen_x;
    int start_row = editor.completion_screen_y + 1;

    if (start_col + popup_width > editor.screen_cols) {
        start_col = editor.screen_cols - popup_width + 1;
    }
    if (start_col < 1) start_col = 1;

    int max_row = editor.screen_rows - 1;
    if (start_row + popup_height > max_row) {
        start_row = editor.completion_screen_y - popup_height - 1;
    }
    if (start_row < 2) start_row = 2;

    for (int y = 0; y < popup_height; y++) {
        render_move_cursor(rb, start_row + y, start_col);
        render_buf_append(rb, STYLE_HOVER_BG);
        for (int x = 0; x < popup_width; x++) {
            render_buf_append(rb, " ");
        }
    }

    int row = start_row + 1;
    int remaining = visible_lines;
    int idx = 0;
    if (show_no_match && remaining > 0) {
        render_move_cursor(rb, row, start_col + 1);
        render_buf_appendf(rb, "%s%s", COLOR_NORMAL, STYLE_HOVER_BG FG_RED);
        render_buf_appendf(rb, "%.*s", content_width, editor.completion_prefix);
        row++;
        remaining--;
    }
    while (remaining > 0 && idx < editor.completion_count) {
        render_move_cursor(rb, row, start_col + 1);
        render_buf_appendf(rb, "%s%s", COLOR_NORMAL, STYLE_HOVER_BG STYLE_HOVER_FG);
        render_completion_line(rb, &editor.completion_items[idx], content_width);
        row++;
        idx++;
        remaining--;
    }

    render_buf_append(rb, COLOR_RESET);
}

static void draw_hover_popup(RenderBuf *rb) {
    if (!editor.hover_active || !editor.hover_text) return;
    if (editor.quit_confirmation_active || editor.reload_confirmation_active) return;

    int max_width = editor.screen_cols - 4;
    if (max_width < 20) return;

    int wrap_width = max_width - 2;
    if (wrap_width < 10) wrap_width = 10;

    int max_line_len = 0;
    int total_lines = 0;
    const char *line = editor.hover_text;

    while (line && *line) {
        const char *next = strchr(line, '\n');
        int line_len = next ? (int)(next - line) : (int)strlen(line);
        if (line_len > max_line_len) {
            max_line_len = line_len > wrap_width ? wrap_width : line_len;
        }
        int wrapped = line_len == 0 ? 1 : (line_len + wrap_width - 1) / wrap_width;
        total_lines += wrapped;
        if (!next) break;
        line = next + 1;
    }
    if (total_lines == 0) total_lines = 1;

    int content_width = max_line_len;
    if (content_width < 20) content_width = 20;
    if (content_width > wrap_width) content_width = wrap_width;

    int popup_width = content_width + 2;
    int popup_height = total_lines + 2;

    int start_col = editor.hover_screen_x;
    int start_row = editor.hover_screen_y + 1;

    if (start_col + popup_width > editor.screen_cols) {
        start_col = editor.screen_cols - popup_width + 1;
    }
    if (start_col < 1) start_col = 1;

    int max_row = editor.screen_rows - 1;
    if (start_row + popup_height > max_row) {
        start_row = editor.hover_screen_y - popup_height - 1;
    }
    if (start_row < 2) start_row = 2;

    for (int y = 0; y < popup_height; y++) {
        render_move_cursor(rb, start_row + y, start_col);
        render_buf_append(rb, STYLE_HOVER_BG);
        for (int x = 0; x < popup_width; x++) {
            render_buf_append(rb, " ");
        }
    }

    int row = start_row + 1;
    line = editor.hover_text;
    while (line && *line && row < start_row + popup_height - 1) {
        const char *next = strchr(line, '\n');
        int line_len = next ? (int)(next - line) : (int)strlen(line);
        if (line_len == 0) {
            render_move_cursor(rb, row, start_col + 1);
            render_buf_appendf(rb, "%s%s", COLOR_NORMAL, STYLE_HOVER_BG STYLE_HOVER_FG);
            row++;
            if (!next) break;
            line = next + 1;
            continue;
        }

        int offset = 0;
        while (offset < line_len && row < start_row + popup_height - 1) {
            int chunk = line_len - offset;
            if (chunk > content_width) chunk = content_width;
            render_move_cursor(rb, row, start_col + 1);
            render_buf_appendf(rb, "%s%s", COLOR_NORMAL, STYLE_HOVER_BG STYLE_HOVER_FG);
            render_buf_appendf(rb, "%.*s", chunk, line + offset);
            offset += chunk;
            row++;
        }

        if (!next) break;
        line = next + 1;
    }

    render_buf_append(rb, COLOR_RESET);
}

void draw_screen(void) {
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

    draw_completion_popup(&rb);
    draw_hover_popup(&rb);
    
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
