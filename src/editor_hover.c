#define _GNU_SOURCE
#include "editor_hover.h"
#include "editor_tabs.h"
#include "editor_folds.h"
#include "lsp.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOVER_DELAY_MS 250

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000LL);
}

static void append_str(char **buf, int *len, int *cap, const char *text) {
    if (!text || !buf || !len || !cap) return;
    int add_len = (int)strlen(text);
    if (add_len == 0) return;
    if (*len + add_len + 1 > *cap) {
        int new_cap = *cap == 0 ? 128 : *cap;
        while (new_cap < *len + add_len + 1) new_cap *= 2;
        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf) return;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, text, add_len);
    *len += add_len;
    (*buf)[*len] = '\0';
}

static void get_cursor_screen_pos(Tab *tab, int *out_row, int *out_col) {
    if (!tab || !out_row || !out_col) return;

    int text_start_col = 1;
    if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
        text_start_col += editor.file_manager_width + 1;
    }

    int visible_lines = 0;
    for (int y = tab->offset_y; y < tab->cursor_y; y++) {
        if (is_line_visible(tab, y)) {
            visible_lines++;
        }
    }

    int screen_row = visible_lines + 2;
    int screen_col = (tab->cursor_x - tab->offset_x) + text_start_col + editor.line_number_width;

    if (screen_row < 2) screen_row = 2;
    if (screen_row >= editor.screen_rows) screen_row = editor.screen_rows - 1;
    if (screen_col < text_start_col + 7) screen_col = text_start_col + 7;

    *out_row = screen_row;
    *out_col = screen_col;
}

static const char *skip_spaces(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static char *build_hover_header(const char *text, int line, int col) {
    if (!text) return NULL;

    const char *p = text;
    const char *first_line_start = NULL;
    const char *first_line_end = NULL;

    while (p && *p) {
        const char *next = strchr(p, '\n');
        int len = next ? (int)(next - p) : (int)strlen(p);
        const char *trim = skip_spaces(p);
        if (len > 0 && *trim != '\0') {
            first_line_start = p;
            first_line_end = p + len;
            break;
        }
        if (!next) break;
        p = next + 1;
    }

    char kind[64] = {0};
    char name[128] = {0};
    if (first_line_start && first_line_end && first_line_end > first_line_start) {
        const char *space = memchr(first_line_start, ' ', (size_t)(first_line_end - first_line_start));
        if (space) {
            int kind_len = (int)(space - first_line_start);
            if (kind_len > 0 && kind_len < (int)sizeof(kind)) {
                memcpy(kind, first_line_start, kind_len);
                kind[kind_len] = '\0';
                const char *name_start = skip_spaces(space + 1);
                int name_len = (int)(first_line_end - name_start);
                if (name_len > 0) {
                    if (name_len >= (int)sizeof(name)) name_len = (int)sizeof(name) - 1;
                    memcpy(name, name_start, name_len);
                    name[name_len] = '\0';
                }
            }
        }
    }

    char header[256];
    if (name[0] != '\0' && kind[0] != '\0') {
        snprintf(header, sizeof(header), "Symbol: %s (%s)\nLocation: line %d, col %d",
                 name, kind, line + 1, col + 1);
    } else {
        snprintf(header, sizeof(header), "Location: line %d, col %d", line + 1, col + 1);
    }

    return strdup(header);
}

static bool contains_word_before(const char *text, const char *end, const char *word) {
    if (!text || !end || !word) return false;
    size_t word_len = strlen(word);
    const char *p = text;
    while (p && p + word_len <= end) {
        const char *match = strstr(p, word);
        if (!match || match + word_len > end) return false;
        bool left_ok = (match == text) || !isalnum((unsigned char)match[-1]);
        bool right_ok = (match + word_len >= end) || !isalnum((unsigned char)match[word_len]);
        if (left_ok && right_ok) return true;
        p = match + word_len;
    }
    return false;
}

static void extract_last_identifier(const char *s, char *out, size_t out_sz) {
    if (!s || !out || out_sz == 0) return;
    out[0] = '\0';
    int len = (int)strlen(s);
    int i = len - 1;
    while (i >= 0 && !(isalnum((unsigned char)s[i]) || s[i] == '_')) i--;
    if (i < 0) return;
    int end = i;
    while (i >= 0 && (isalnum((unsigned char)s[i]) || s[i] == '_')) i--;
    int start = i + 1;
    int id_len = end - start + 1;
    if (id_len <= 0) return;
    if ((size_t)id_len >= out_sz) id_len = (int)out_sz - 1;
    memcpy(out, s + start, id_len);
    out[id_len] = '\0';
}

static char *append_hover_members(const char *text) {
    if (!text) return NULL;
    const char *brace = strchr(text, '{');
    if (!brace) return strdup(text);
    const char *end = strchr(brace + 1, '}');
    if (!end || end <= brace + 1) return strdup(text);

    bool is_enum = contains_word_before(text, brace, "enum");
    bool is_struct = contains_word_before(text, brace, "struct") ||
                     contains_word_before(text, brace, "class") ||
                     contains_word_before(text, brace, "union");
    if (!is_enum && !is_struct) return strdup(text);

    char *segment = strndup(brace + 1, (size_t)(end - brace - 1));
    if (!segment) return strdup(text);

    char *out = NULL;
    int out_len = 0;
    int out_cap = 0;
    append_str(&out, &out_len, &out_cap, text);

    int count = 0;
    const int max_items = 12;
    char *p = segment;
    char *token_start = segment;
    for (; ; p++) {
        if (*p == '\0' || *p == '\n' || *p == ';' || *p == ',') {
            char saved = *p;
            *p = '\0';
            char *token = token_start;
            char *comment = strstr(token, "//");
            if (comment) *comment = '\0';
            while (*token && isspace((unsigned char)*token)) token++;
            char *tail = token + strlen(token);
            while (tail > token && isspace((unsigned char)tail[-1])) tail--;
            *tail = '\0';
            if (*token) {
                char ident[64];
                extract_last_identifier(token, ident, sizeof(ident));
                if (ident[0] != '\0') {
                    if (count == 0) {
                        append_str(&out, &out_len, &out_cap, "\n\n");
                        append_str(&out, &out_len, &out_cap, is_enum ? "Variants:" : "Fields:");
                    }
                    if (count < max_items) {
                        append_str(&out, &out_len, &out_cap, "\n- ");
                        append_str(&out, &out_len, &out_cap, ident);
                    }
                    count++;
                }
            }
            if (saved == '\0') break;
            *p = saved;
            token_start = p + 1;
        }
    }

    if (count > max_items) {
        append_str(&out, &out_len, &out_cap, "\n- ...");
    }

    free(segment);
    if (!out) return strdup(text);
    return out;
}

void hover_clear(void) {
    if (editor.hover_text) {
        free(editor.hover_text);
        editor.hover_text = NULL;
    }
    if (editor.hover_active) {
        editor.needs_full_redraw = true;
    }
    editor.hover_active = false;
    editor.hover_request_active = false;
    editor.hover_pending = false;
}

void hover_schedule_request(int buffer_line, int buffer_col, int screen_x, int screen_y) {
    if (buffer_line < 0 || buffer_col < 0) return;

    if (editor.hover_target_line != buffer_line || editor.hover_target_col != buffer_col ||
        editor.hover_screen_x != screen_x || editor.hover_screen_y != screen_y) {
        hover_clear();
    }

    editor.hover_target_line = buffer_line;
    editor.hover_target_col = buffer_col;
    editor.hover_screen_x = screen_x;
    editor.hover_screen_y = screen_y;
    editor.hover_last_move_ms = monotonic_ms();
    editor.hover_pending = true;
}

void hover_show_diagnostic(int buffer_line, int screen_x, int screen_y) {
    if (buffer_line < 0) return;

    const char *msg = get_line_diagnostic_message(get_current_tab(), buffer_line);
    if (!msg || msg[0] == '\0') {
        hover_clear();
        return;
    }

    const char *sev_str = "Info";
    DiagnosticSeverity sev = get_line_diagnostic_severity(get_current_tab(), buffer_line);
    if (sev == DIAG_ERROR) sev_str = "Error";
    else if (sev == DIAG_WARNING) sev_str = "Warning";
    else if (sev == DIAG_HINT) sev_str = "Hint";

    hover_clear();
    editor.hover_screen_x = screen_x;
    editor.hover_screen_y = screen_y;
    editor.hover_target_line = buffer_line;
    editor.hover_target_col = 0;
    editor.hover_pending = false;
    editor.hover_request_active = false;

    size_t total_len = strlen(sev_str) + 2 + strlen(msg) + 1;
    editor.hover_text = malloc(total_len);
    if (editor.hover_text) {
        snprintf(editor.hover_text, total_len, "%s: %s", sev_str, msg);
        editor.hover_active = true;
        editor.needs_full_redraw = true;
    }
}

void hover_process_requests(void) {
    if (!editor.hover_pending) return;
    if (monotonic_ms() - editor.hover_last_move_ms < HOVER_DELAY_MS) return;

    Tab *tab = get_current_tab();
    if (editor.quit_confirmation_active || editor.reload_confirmation_active ||
        editor.file_manager_focused || editor.mouse_dragging) {
        editor.hover_pending = false;
        return;
    }

    if (!tab || !editor.lsp_enabled || !tab->lsp_opened || !tab->filename) {
        editor.hover_pending = false;
        return;
    }

    editor.hover_pending = false;
    editor.hover_request_line = editor.hover_target_line;
    editor.hover_request_col = editor.hover_target_col;
    editor.hover_request_ms = monotonic_ms();
    editor.hover_request_active = true;
    lsp_request_hover(tab->filename, editor.hover_request_line, editor.hover_request_col);
}

void hover_request_cursor(Tab *tab) {
    if (!tab || !tab->filename) return;
    int row = 0;
    int col = 0;
    get_cursor_screen_pos(tab, &row, &col);
    editor.hover_screen_x = col;
    editor.hover_screen_y = row;
    editor.hover_target_line = tab->cursor_y;
    editor.hover_target_col = tab->cursor_x;
    editor.hover_request_line = tab->cursor_y;
    editor.hover_request_col = tab->cursor_x;
    editor.hover_request_ms = monotonic_ms();
    editor.hover_request_active = true;
    lsp_request_hover(tab->filename, tab->cursor_y, tab->cursor_x);
}

void lsp_hover_handler(const char *uri, int line, int col, const char *text) {
    if (!uri) return;

    char *path = lsp_uri_to_path(uri);
    if (!path) return;

    int tab_idx = find_tab_with_file(path);
    free(path);

    editor.hover_request_active = false;
    if (tab_idx < 0) return;
    if (tab_idx != editor.current_tab) return;
    if (line != editor.hover_request_line || col != editor.hover_request_col) return;

    hover_clear();

    if (!text || text[0] == '\0') {
        editor.needs_full_redraw = true;
        return;
    }

    char *header = build_hover_header(text, line, col);
    char *combined = NULL;
    if (header) {
        int total_len = (int)strlen(header) + 2 + (int)strlen(text) + 1;
        combined = malloc((size_t)total_len);
        if (combined) {
            snprintf(combined, (size_t)total_len, "%s\n\n%s", header, text);
        }
        free(header);
    }
    char *augmented = append_hover_members(combined ? combined : text);
    if (combined) free(combined);
    editor.hover_text = augmented ? augmented : strdup(text);
    editor.hover_active = editor.hover_text != NULL;
    editor.needs_full_redraw = true;
}
