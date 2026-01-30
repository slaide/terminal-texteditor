#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>

typedef struct {
    char *data;
    int len;
    int cap;
} RenderBuf;

void render_buf_init(RenderBuf *rb);
void render_buf_free(RenderBuf *rb);
bool render_buf_ensure(RenderBuf *rb, int extra);
void render_buf_append(RenderBuf *rb, const char *s);
void render_buf_append_char(RenderBuf *rb, char c);
void render_buf_appendf(RenderBuf *rb, const char *fmt, ...);
void render_move_cursor(RenderBuf *rb, int row, int col);
void render_clear_screen(RenderBuf *rb);

void draw_screen(void);
void draw_line(int screen_y, int file_y, int start_col);

#endif
