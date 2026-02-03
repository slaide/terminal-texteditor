#define _GNU_SOURCE
#include "editor_folds.h"
#include "editor_config.h"
#include <stdlib.h>
#include <string.h>

void clear_tab_folds(Tab *tab) {
    if (!tab) return;
    free(tab->folds);
    tab->folds = NULL;
    tab->fold_count = 0;
    tab->fold_capacity = 0;
}

void add_fold(Tab *tab, int start_line, int end_line) {
    if (!tab || start_line >= end_line) return;

    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].start_line == start_line) return;
    }

    if (tab->fold_count >= tab->fold_capacity) {
        int new_capacity = tab->fold_capacity == 0 ? 16 : tab->fold_capacity * 2;
        Fold *new_folds = realloc(tab->folds, new_capacity * sizeof(Fold));
        if (!new_folds) return;
        tab->folds = new_folds;
        tab->fold_capacity = new_capacity;
    }

    tab->folds[tab->fold_count].start_line = start_line;
    tab->folds[tab->fold_count].end_line = end_line;
    tab->folds[tab->fold_count].is_folded = false;
    tab->fold_count++;
}

void detect_folds_braces(Tab *tab) {
    if (!tab || !tab->buffer) return;

    int stack[1024];
    int stack_size = 0;

    for (int i = 0; i < tab->buffer->line_count; i++) {
        char *line = tab->buffer->lines[i];
        if (!line) continue;

        for (char *p = line; *p; p++) {
            if (*p == '{') {
                if (stack_size < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    stack[stack_size++] = i;
                }
            } else if (*p == '}') {
                if (stack_size > 0) {
                    int start = stack[--stack_size];
                    if (i > start) {
                        add_fold(tab, start, i);
                    }
                }
            }
        }
    }
}

static int get_line_indent(const char *line) {
    int indent = 0;
    while (line && *line) {
        if (*line == ' ') indent++;
        else if (*line == '\t') indent += 4;
        else break;
        line++;
    }
    return indent;
}

void detect_folds_indent(Tab *tab) {
    if (!tab || !tab->buffer) return;

    int stack_line[1024];
    int stack_indent[1024];
    int stack_size = 0;

    for (int i = 0; i < tab->buffer->line_count; i++) {
        char *line = tab->buffer->lines[i];
        if (!line) continue;

        int indent = get_line_indent(line);
        if (*line == '\0') continue;

        while (stack_size > 0 && indent <= stack_indent[stack_size - 1]) {
            int start = stack_line[--stack_size];
            if (i - 1 > start) {
                add_fold(tab, start, i - 1);
            }
        }

        if (stack_size < (int)(sizeof(stack_line) / sizeof(stack_line[0]))) {
            stack_line[stack_size] = i;
            stack_indent[stack_size] = indent;
            stack_size++;
        }
    }

    while (stack_size > 0) {
        int start = stack_line[--stack_size];
        int end = tab->buffer->line_count - 1;
        if (end > start) {
            add_fold(tab, start, end);
        }
    }
}

static int get_heading_level(const char *line) {
    if (!line || line[0] != '#') return 0;
    int level = 0;
    while (*line == '#') {
        level++;
        line++;
    }
    return level;
}

static bool is_code_fence(const char *line) {
    if (!line) return false;
    return (strncmp(line, "```", 3) == 0 || strncmp(line, "~~~", 3) == 0);
}

void detect_folds_headings(Tab *tab) {
    if (!tab || !tab->buffer) return;

    int stack_line[128];
    int stack_level[128];
    int stack_size = 0;
    bool in_code_block = false;

    for (int i = 0; i < tab->buffer->line_count; i++) {
        char *line = tab->buffer->lines[i];
        if (!line) continue;

        if (is_code_fence(line)) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) continue;

        int level = get_heading_level(line);
        if (level == 0) continue;

        while (stack_size > 0 && level <= stack_level[stack_size - 1]) {
            int start = stack_line[--stack_size];
            if (i - 1 > start) {
                add_fold(tab, start, i - 1);
            }
        }

        if (stack_size < (int)(sizeof(stack_line) / sizeof(stack_line[0]))) {
            stack_line[stack_size] = i;
            stack_level[stack_size] = level;
            stack_size++;
        }
    }

    while (stack_size > 0) {
        int start = stack_line[--stack_size];
        int end = tab->buffer->line_count - 1;
        if (end > start) {
            add_fold(tab, start, end);
        }
    }
}

void detect_folds(Tab *tab) {
    if (!tab) return;
    clear_tab_folds(tab);

    ConfigFoldStyle style = tab->fold_style;
    if (style == FOLD_STYLE_BRACES) {
        detect_folds_braces(tab);
    } else if (style == FOLD_STYLE_INDENT) {
        detect_folds_indent(tab);
    } else if (style == FOLD_STYLE_HEADINGS) {
        detect_folds_headings(tab);
    }
}

Fold *get_fold_at_line(Tab *tab, int line) {
    if (!tab) return NULL;
    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].start_line == line) {
            return &tab->folds[i];
        }
    }
    return NULL;
}

Fold *get_fold_containing_line(Tab *tab, int line) {
    if (!tab) return NULL;
    for (int i = 0; i < tab->fold_count; i++) {
        if (line > tab->folds[i].start_line && line < tab->folds[i].end_line) {
            return &tab->folds[i];
        }
    }
    return NULL;
}

bool is_line_visible(Tab *tab, int line) {
    Fold *fold = get_fold_containing_line(tab, line);
    return !(fold && fold->is_folded);
}

int get_next_visible_line(Tab *tab, int line) {
    if (!tab) return line;
    for (int i = line + 1; i < tab->buffer->line_count; i++) {
        if (is_line_visible(tab, i)) return i;
    }
    return line;
}

int get_prev_visible_line(Tab *tab, int line) {
    if (!tab) return line;
    for (int i = line - 1; i >= 0; i--) {
        if (is_line_visible(tab, i)) return i;
    }
    return line;
}

void toggle_fold_at_line(Tab *tab, int line) {
    Fold *fold = get_fold_at_line(tab, line);
    if (!fold) return;
    fold->is_folded = !fold->is_folded;
    editor.needs_full_redraw = true;
}

int count_folded_lines_before(Tab *tab, int line) {
    if (!tab) return 0;
    int count = 0;
    for (int i = 0; i < tab->fold_count; i++) {
        if (tab->folds[i].is_folded && tab->folds[i].start_line < line) {
            if (tab->folds[i].end_line < line) {
                count += tab->folds[i].end_line - tab->folds[i].start_line - 1;
            } else {
                count += line - tab->folds[i].start_line - 1;
            }
        }
    }
    return count;
}

int file_line_to_display_line(Tab *tab, int file_line) {
    if (!tab) return file_line;
    return file_line - count_folded_lines_before(tab, file_line);
}

int display_line_to_file_line(Tab *tab, int display_line) {
    if (!tab) return display_line;

    int file_line = 0;
    int current_display = 0;

    while (current_display < display_line && file_line < tab->buffer->line_count) {
        if (is_line_visible(tab, file_line)) {
            current_display++;
        }
        file_line++;
    }

    while (file_line < tab->buffer->line_count && !is_line_visible(tab, file_line)) {
        file_line++;
    }

    return file_line;
}
