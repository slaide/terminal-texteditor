#ifndef EDITOR_COMPLETION_H
#define EDITOR_COMPLETION_H

#include "editor.h"
#include "lsp.h"

void completion_clear(void);
void completion_request_at_cursor(Tab *tab, const char *trigger, int trigger_kind, bool keep_items);
bool completion_has_member_context(Tab *tab);
void lsp_completion_handler(const char *uri, int line, int col,
                            LspCompletionItem *items, int count);

#endif
