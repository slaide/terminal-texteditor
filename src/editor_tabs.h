#ifndef EDITOR_TABS_H
#define EDITOR_TABS_H

#include "editor.h"

Tab* get_current_tab(void);
int create_new_tab(const char* filename);
void free_tab(Tab* tab);
void close_tab(int tab_index);
void switch_to_tab(int tab_index);
void switch_to_next_tab(void);
void switch_to_prev_tab(void);
int find_tab_with_file(const char* filename);

#endif
