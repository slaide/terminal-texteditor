#ifndef LSP_INTEGRATION_H
#define LSP_INTEGRATION_H

#include "buffer.h"
#include "editor.h"
#include "lsp.h"

char *get_buffer_content(TextBuffer *buffer);

void clear_tab_diagnostics(Tab *tab);
void clear_tab_tokens(Tab *tab);

void lsp_diagnostics_handler(const char *uri, Diagnostic *diags, int count);
void lsp_semantic_tokens_handler(const char *uri, SemanticToken *tokens, int count);

void notify_lsp_file_opened(Tab *tab);
void notify_lsp_file_changed(Tab *tab);
void notify_lsp_file_closed(Tab *tab);

void request_semantic_tokens(Tab *tab);
void schedule_semantic_tokens(Tab *tab);
void process_semantic_tokens_requests(void);

const char *get_token_color(SemanticTokenType type);

#endif
