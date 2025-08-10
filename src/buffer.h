#ifndef BUFFER_H
#define BUFFER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **lines;
    int line_count;
    int line_capacity;
} TextBuffer;

TextBuffer *buffer_create(void);
void buffer_free(TextBuffer *buffer);
bool buffer_load_from_file(TextBuffer *buffer, const char *filename);
bool buffer_save_to_file(TextBuffer *buffer, const char *filename);
void buffer_insert_char(TextBuffer *buffer, int row, int col, char c);
void buffer_delete_char(TextBuffer *buffer, int row, int col);
void buffer_insert_newline(TextBuffer *buffer, int row, int col);
void buffer_insert_line(TextBuffer *buffer, int row, const char *text);
void buffer_delete_line(TextBuffer *buffer, int row);
void buffer_merge_lines(TextBuffer *buffer, int row);
char *buffer_get_text_range(TextBuffer *buffer, int start_row, int start_col, int end_row, int end_col);

#endif