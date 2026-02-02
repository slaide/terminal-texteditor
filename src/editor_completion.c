#define _GNU_SOURCE
#include "editor_completion.h"
#include "editor_folds.h"
#include "editor_tabs.h"
#include "lsp.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000LL);
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

static void completion_free_items(void) {
    if (!editor.completion_items) return;
    for (int i = 0; i < editor.completion_count; i++) {
        free(editor.completion_items[i].label);
        free(editor.completion_items[i].detail);
        free(editor.completion_items[i].doc);
    }
    free(editor.completion_items);
    editor.completion_items = NULL;
    editor.completion_count = 0;
    editor.completion_capacity = 0;
}

static bool is_word_char_local(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static char *completion_prefix_at(Tab *tab, int line, int col) {
    if (!tab || !tab->buffer) return NULL;
    if (line < 0 || line >= tab->buffer->line_count) return NULL;
    char *text = tab->buffer->lines[line];
    if (!text) return NULL;
    int len = (int)strlen(text);
    int idx = col - 1;
    if (idx < 0 || idx >= len) return NULL;
    if (!is_word_char_local(text[idx])) return NULL;
    int end = idx + 1;
    int start = idx;
    while (start > 0 && is_word_char_local(text[start - 1])) start--;
    int out_len = end - start;
    if (out_len <= 0) return NULL;
    char *out = malloc((size_t)out_len + 1);
    if (!out) return NULL;
    memcpy(out, text + start, (size_t)out_len);
    out[out_len] = '\0';
    return out;
}

bool completion_has_member_context(Tab *tab) {
    if (!tab || !tab->buffer) return false;
    int line = tab->cursor_y;
    int col = tab->cursor_x;
    if (line < 0 || line >= tab->buffer->line_count) return false;
    char *text = tab->buffer->lines[line];
    if (!text) return false;
    int len = (int)strlen(text);
    int idx = col - 1;
    if (idx < 0 || idx >= len) return false;

    while (idx >= 0 && is_word_char_local(text[idx])) idx--;
    if (idx < 0) return false;
    return text[idx] == '.';
}

static char *first_doc_line(const char *doc) {
    if (!doc) return NULL;
    const char *start = doc;
    while (*start && isspace((unsigned char)*start)) start++;
    if (*start == '\0') return NULL;

    const char *end = start;
    while (*end && *end != '\n' && *end != '\r') end++;
    const char *trim = end;
    while (trim > start && isspace((unsigned char)trim[-1])) trim--;

    int len = (int)(trim - start);
    if (len <= 0) return NULL;

    char *out = malloc((size_t)len + 1);
    if (!out) return NULL;
    memcpy(out, start, (size_t)len);
    out[len] = '\0';
    return out;
}

void completion_clear(void) {
    completion_free_items();
    if (editor.completion_prefix) {
        free(editor.completion_prefix);
        editor.completion_prefix = NULL;
    }
    editor.completion_prefix_match = true;
    editor.completion_active = false;
    editor.completion_request_active = false;
}

void completion_request_at_cursor(Tab *tab, const char *trigger, int trigger_kind, bool keep_items) {
    if (!tab || !tab->filename || !tab->lsp_opened) return;
    if (!editor.lsp_enabled || !lsp_completion_is_supported()) return;

    if (!keep_items) {
        completion_clear();
    }

    int row = 0;
    int col = 0;
    get_cursor_screen_pos(tab, &row, &col);

    editor.completion_screen_x = col;
    editor.completion_screen_y = row;
    editor.completion_request_line = tab->cursor_y;
    editor.completion_request_col = tab->cursor_x;
    editor.completion_request_ms = monotonic_ms();
    editor.completion_request_active = true;

    lsp_request_completion(tab->filename, tab->cursor_y, tab->cursor_x, trigger, trigger_kind);
}

void lsp_completion_handler(const char *uri, int line, int col,
                            LspCompletionItem *items, int count) {
    (void)uri;
    if (!editor.completion_request_active) return;
    editor.completion_request_active = false;

    if (line != editor.completion_request_line || col != editor.completion_request_col) {
        return;
    }

    completion_free_items();

    if (!items || count <= 0) {
        editor.completion_active = false;
        return;
    }

    editor.completion_items = calloc(count, sizeof(CompletionEntry));
    if (!editor.completion_items) return;
    editor.completion_capacity = count;
    editor.completion_count = 0;

    for (int i = 0; i < count; i++) {
        if (!items[i].label || items[i].label[0] == '\0') continue;
        CompletionEntry *entry = &editor.completion_items[editor.completion_count++];
        entry->label = strdup(items[i].label);
        entry->detail = items[i].detail ? strdup(items[i].detail) : NULL;
        entry->doc = first_doc_line(items[i].documentation);
    }

    editor.completion_active = editor.completion_count > 0;

    if (editor.completion_prefix) {
        free(editor.completion_prefix);
        editor.completion_prefix = NULL;
    }
    editor.completion_prefix_match = true;

    Tab *tab = get_current_tab();
    char *prefix = completion_prefix_at(tab, line, col);
    if (prefix && prefix[0] != '\0') {
        bool match = false;
        for (int i = 0; i < editor.completion_count; i++) {
            if (editor.completion_items[i].label &&
                strncmp(editor.completion_items[i].label, prefix, strlen(prefix)) == 0) {
                match = true;
                break;
            }
        }
        editor.completion_prefix = prefix;
        editor.completion_prefix_match = match;
    } else if (prefix) {
        free(prefix);
    }
}
