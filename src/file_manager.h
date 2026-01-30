#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "render.h"

void refresh_file_list(void);
void toggle_file_manager(void);
void draw_file_manager(RenderBuf *rb);
void file_manager_navigate(int direction);
void file_manager_select_item(void);
void free_file_list(void);

#endif
