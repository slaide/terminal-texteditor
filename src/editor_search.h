#ifndef EDITOR_SEARCH_H
#define EDITOR_SEARCH_H

void enter_find_mode(void);
void exit_find_mode(void);
int find_matches(void);
void jump_to_match(int match_num);
void find_next(void);
void find_previous(void);

#endif
