#define _GNU_SOURCE
#include "editor_search.h"
#include "editor.h"
#include "editor_tabs.h"
#include "editor_cursor.h"
#include "editor_selection.h"
#include <stdlib.h>
#include <string.h>

void enter_find_mode(void) {
    editor.find_mode = true;
    clear_selection();
    if (!editor.search_query) {
        editor.search_query_capacity = 256;
        editor.search_query = malloc(editor.search_query_capacity);
        editor.search_query[0] = '\0';
    }
    editor.search_query_len = 0;
    editor.current_match = 0;
    editor.total_matches = 0;
    editor.needs_full_redraw = true;
}

void exit_find_mode(void) {
    editor.find_mode = false;
    editor.needs_full_redraw = true;
}

int find_matches(void) {
    Tab* tab = get_current_tab();
    if (!tab || !editor.search_query || editor.search_query_len == 0) {
        editor.total_matches = 0;
        editor.current_match = 0;
        return 0;
    }
    
    int matches = 0;
    int current_found = 0;
    bool found_current = false;
    
    for (int y = 0; y < tab->buffer->line_count; y++) {
        char *line = tab->buffer->lines[y];
        if (!line) continue;
        
        char *pos = line;
        while ((pos = strstr(pos, editor.search_query)) != NULL) {
            matches++;
            int x = pos - line;
            
            if (!found_current && (y > tab->cursor_y || 
                (y == tab->cursor_y && x >= tab->cursor_x))) {
                current_found = matches;
                found_current = true;
            }
            
            pos++;
        }
    }
    
    editor.total_matches = matches;
    editor.current_match = found_current ? current_found : (matches > 0 ? 1 : 0);
    return matches;
}

void jump_to_match(int match_num) {
    Tab* tab = get_current_tab();
    if (!tab || match_num < 1 || match_num > editor.total_matches) return;
    
    int found = 0;
    for (int y = 0; y < tab->buffer->line_count; y++) {
        char *line = tab->buffer->lines[y];
        if (!line) continue;
        
        char *pos = line;
        while ((pos = strstr(pos, editor.search_query)) != NULL) {
            found++;
            if (found == match_num) {
                int x = pos - line;
                
                tab->cursor_x = x;
                tab->cursor_y = y;
                
                tab->select_start_x = x;
                tab->select_start_y = y;
                tab->select_end_x = x + editor.search_query_len;
                tab->select_end_y = y;
                tab->selecting = true;
                
                scroll_if_needed();
                editor.current_match = match_num;
                editor.needs_full_redraw = true;
                return;
            }
            pos++;
        }
    }
}

void find_next(void) {
    if (editor.total_matches == 0) return;
    
    int next = editor.current_match + 1;
    if (next > editor.total_matches) next = 1;
    jump_to_match(next);
}

void find_previous(void) {
    if (editor.total_matches == 0) return;
    
    int prev = editor.current_match - 1;
    if (prev < 1) prev = editor.total_matches;
    jump_to_match(prev);
}
