#define _GNU_SOURCE
#include "lsp_integration.h"
#include "editor_config.h"
#include "terminal.h"
#include "editor_completion.h"
#include "editor_tabs.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define SEMANTIC_TOKENS_DELAY_MS 150

void lsp_hover_handler(const char *uri, int line, int col, const char *text);

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000LL);
}

char *get_buffer_content(TextBuffer *buffer) {
    if (!buffer || buffer->line_count <= 0) return NULL;

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

DiagnosticSeverity get_line_diagnostic_severity(Tab *tab, int line) {
    if (!tab || !tab->diagnostics) return 0;

    DiagnosticSeverity worst = 0;
    for (int i = 0; i < tab->diagnostic_count; i++) {
        if (tab->diagnostics[i].line == line) {
            if (worst == 0 || tab->diagnostics[i].severity < worst) {
                worst = tab->diagnostics[i].severity;
            }
        }
    }
    return worst;
}

const char *get_line_diagnostic_message(Tab *tab, int line) {
    if (!tab || !tab->diagnostics) return NULL;

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
        lsp_set_hover_callback(lsp_hover_handler);
        lsp_set_completion_callback(lsp_completion_handler);
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
        schedule_semantic_tokens(tab);
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

// Semantic token functions

void clear_tab_tokens(Tab *tab) {
    if (!tab) return;
    if (tab->tokens) {
        free(tab->tokens);
        tab->tokens = NULL;
    }
    tab->token_count = 0;
    tab->token_capacity = 0;
    if (tab->token_line_start) {
        free(tab->token_line_start);
        tab->token_line_start = NULL;
    }
    if (tab->token_line_count) {
        free(tab->token_line_count);
        tab->token_line_count = NULL;
    }
    tab->token_line_capacity = 0;
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

void schedule_semantic_tokens(Tab *tab) {
    if (!tab || !editor.lsp_enabled || !tab->lsp_opened) return;
    tab->tokens_pending = true;
    tab->tokens_last_change_ms = monotonic_ms();
}

void process_semantic_tokens_requests(void) {
    if (!editor.lsp_enabled) return;
    for (int i = 0; i < editor.tab_count; i++) {
        Tab *tab = &editor.tabs[i];
        if (!tab->tokens_pending) continue;
        if (!tab->lsp_opened || !tab->filename) {
            tab->tokens_pending = false;
            continue;
        }
        if (monotonic_ms() - tab->tokens_last_change_ms < SEMANTIC_TOKENS_DELAY_MS) {
            continue;
        }
        tab->tokens_pending = false;
        request_semantic_tokens(tab);
    }
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
        default:
            return COLOR_RESET;
    }
}
