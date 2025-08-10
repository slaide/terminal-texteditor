#define _GNU_SOURCE
#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

TextBuffer *buffer_create(void) {
    TextBuffer *buffer = malloc(sizeof(TextBuffer));
    if (!buffer) return NULL;
    
    buffer->lines = NULL;
    buffer->line_count = 0;
    buffer->line_capacity = 0;
    
    return buffer;
}

void buffer_free(TextBuffer *buffer) {
    if (!buffer) return;
    
    for (int i = 0; i < buffer->line_count; i++) {
        free(buffer->lines[i]);
    }
    free(buffer->lines);
    free(buffer);
}

static bool buffer_ensure_capacity(TextBuffer *buffer, int needed_lines) {
    if (needed_lines <= buffer->line_capacity) return true;
    
    int new_capacity = buffer->line_capacity == 0 ? 8 : buffer->line_capacity * 2;
    while (new_capacity < needed_lines) {
        new_capacity *= 2;
    }
    
    char **new_lines = realloc(buffer->lines, new_capacity * sizeof(char*));
    if (!new_lines) return false;
    
    buffer->lines = new_lines;
    buffer->line_capacity = new_capacity;
    return true;
}

bool buffer_load_from_file(TextBuffer *buffer, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) return false;
    
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    
    while ((nread = getline(&line, &len, file)) != -1) {
        if (line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
        }
        buffer_insert_line(buffer, buffer->line_count, line);
    }
    
    free(line);
    fclose(file);
    
    if (buffer->line_count == 0) {
        buffer_insert_line(buffer, 0, "");
    }
    
    return true;
}

bool buffer_save_to_file(TextBuffer *buffer, const char *filename) {
    FILE *file = fopen(filename, "w");
    if (!file) return false;
    
    for (int i = 0; i < buffer->line_count; i++) {
        if (buffer->lines[i]) {
            fprintf(file, "%s", buffer->lines[i]);
        }
        if (i < buffer->line_count - 1) {
            fprintf(file, "\n");
        }
    }
    
    fclose(file);
    return true;
}

void buffer_insert_char(TextBuffer *buffer, int row, int col, char c) {
    if (row < 0 || row >= buffer->line_count) return;
    
    char *line = buffer->lines[row];
    int len = line ? strlen(line) : 0;
    
    if (col < 0) col = 0;
    if (col > len) col = len;
    
    char *new_line = malloc(len + 2);
    if (!new_line) return;
    
    if (line) {
        memcpy(new_line, line, col);
        new_line[col] = c;
        memcpy(new_line + col + 1, line + col, len - col);
    } else {
        new_line[0] = c;
    }
    new_line[len + 1] = '\0';
    
    free(buffer->lines[row]);
    buffer->lines[row] = new_line;
}

void buffer_delete_char(TextBuffer *buffer, int row, int col) {
    if (row < 0 || row >= buffer->line_count) return;
    
    char *line = buffer->lines[row];
    if (!line) return;
    
    int len = strlen(line);
    if (col < 0 || col >= len) return;
    
    memmove(line + col, line + col + 1, len - col);
}

void buffer_insert_newline(TextBuffer *buffer, int row, int col) {
    if (row < 0 || row >= buffer->line_count) return;
    
    char *line = buffer->lines[row];
    int len = line ? strlen(line) : 0;
    
    if (col < 0) col = 0;
    if (col > len) col = len;
    
    if (!buffer_ensure_capacity(buffer, buffer->line_count + 1)) return;
    
    memmove(buffer->lines + row + 2, buffer->lines + row + 1, 
            (buffer->line_count - row - 1) * sizeof(char*));
    
    char *first_part = malloc(col + 1);
    char *second_part = malloc(len - col + 1);
    
    if (line) {
        memcpy(first_part, line, col);
        memcpy(second_part, line + col, len - col);
    }
    first_part[col] = '\0';
    second_part[len - col] = '\0';
    
    free(buffer->lines[row]);
    buffer->lines[row] = first_part;
    buffer->lines[row + 1] = second_part;
    buffer->line_count++;
}

void buffer_insert_line(TextBuffer *buffer, int row, const char *text) {
    if (row < 0 || row > buffer->line_count) return;
    
    if (!buffer_ensure_capacity(buffer, buffer->line_count + 1)) return;
    
    memmove(buffer->lines + row + 1, buffer->lines + row, 
            (buffer->line_count - row) * sizeof(char*));
    
    buffer->lines[row] = strdup(text ? text : "");
    buffer->line_count++;
}

void buffer_delete_line(TextBuffer *buffer, int row) {
    if (row < 0 || row >= buffer->line_count) return;
    
    free(buffer->lines[row]);
    memmove(buffer->lines + row, buffer->lines + row + 1, 
            (buffer->line_count - row - 1) * sizeof(char*));
    buffer->line_count--;
}

void buffer_merge_lines(TextBuffer *buffer, int row) {
    if (row < 0 || row >= buffer->line_count - 1) return;
    
    char *first = buffer->lines[row];
    char *second = buffer->lines[row + 1];
    
    int first_len = first ? strlen(first) : 0;
    int second_len = second ? strlen(second) : 0;
    
    char *merged = malloc(first_len + second_len + 1);
    if (!merged) return;
    
    if (first) strcpy(merged, first);
    else merged[0] = '\0';
    
    if (second) strcat(merged, second);
    
    free(buffer->lines[row]);
    buffer->lines[row] = merged;
    
    buffer_delete_line(buffer, row + 1);
}

char *buffer_get_text_range(TextBuffer *buffer, int start_row, int start_col, int end_row, int end_col) {
    if (start_row < 0 || start_row >= buffer->line_count ||
        end_row < 0 || end_row >= buffer->line_count ||
        start_row > end_row) return NULL;
    
    if (start_row == end_row) {
        char *line = buffer->lines[start_row];
        if (!line) return strdup("");
        
        int len = strlen(line);
        if (start_col >= len) return strdup("");
        if (end_col > len) end_col = len;
        if (start_col > end_col) return strdup("");
        
        char *result = malloc(end_col - start_col + 1);
        memcpy(result, line + start_col, end_col - start_col);
        result[end_col - start_col] = '\0';
        return result;
    }
    
    size_t total_size = 0;
    for (int i = start_row; i <= end_row; i++) {
        char *line = buffer->lines[i];
        if (line) total_size += strlen(line);
        if (i < end_row) total_size++;
    }
    total_size++;
    
    char *result = malloc(total_size);
    if (!result) return NULL;
    
    result[0] = '\0';
    
    for (int i = start_row; i <= end_row; i++) {
        char *line = buffer->lines[i];
        if (!line) continue;
        
        int len = strlen(line);
        if (i == start_row && i == end_row) {
            int copy_start = (start_col < len) ? start_col : len;
            int copy_end = (end_col < len) ? end_col : len;
            if (copy_start < copy_end) {
                strncat(result, line + copy_start, copy_end - copy_start);
            }
        } else if (i == start_row) {
            if (start_col < len) {
                strcat(result, line + start_col);
            }
            strcat(result, "\n");
        } else if (i == end_row) {
            if (end_col > 0 && len > 0) {
                int copy_end = (end_col < len) ? end_col : len;
                strncat(result, line, copy_end);
            }
        } else {
            strcat(result, line);
            strcat(result, "\n");
        }
    }
    
    return result;
}