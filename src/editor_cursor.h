#ifndef EDITOR_CURSOR_H
#define EDITOR_CURSOR_H

void move_cursor(int dx, int dy);
void scroll_if_needed(void);
void auto_scroll_during_selection(int screen_y);
bool is_word_char(char c);
void move_cursor_word_left(void);
void move_cursor_word_right(void);

#endif
