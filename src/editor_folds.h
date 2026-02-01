#ifndef EDITOR_FOLDS_H
#define EDITOR_FOLDS_H

#include "editor.h"

void clear_tab_folds(Tab *tab);
void detect_folds(Tab *tab);
Fold *get_fold_at_line(Tab *tab, int line);
Fold *get_fold_containing_line(Tab *tab, int line);
bool is_line_visible(Tab *tab, int line);
int get_next_visible_line(Tab *tab, int line);
int get_prev_visible_line(Tab *tab, int line);
void toggle_fold_at_line(Tab *tab, int line);
int file_line_to_display_line(Tab *tab, int file_line);
int display_line_to_file_line(Tab *tab, int display_line);

#endif
