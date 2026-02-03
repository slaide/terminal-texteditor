#ifndef EDITOR_HOVER_H
#define EDITOR_HOVER_H

#include "editor.h"

void hover_process_requests(void);
void hover_schedule_request(int buffer_line, int buffer_col, int screen_x, int screen_y);
void hover_show_diagnostic(int buffer_line, int screen_x, int screen_y);
void hover_clear(void);
void hover_request_cursor(Tab *tab);
void lsp_hover_handler(const char *uri, int line, int col, const char *text);

#endif
