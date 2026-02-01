#ifndef EDITOR_FILES_H
#define EDITOR_FILES_H

#include <stdbool.h>
#include <time.h>

void save_file(void);
void insert_char(char c);
void delete_char(void);
void insert_newline(void);

void enter_filename_input_mode(void);
void exit_filename_input_mode(void);
void process_filename_input(void);

int get_file_size(void);
const char* format_file_size(int bytes);
const char* get_file_size_str(long size, bool is_dir);

bool is_directory(const char* filepath);
bool has_unsaved_changes(void);
time_t get_file_mtime(const char* filename);
void check_file_changes(void);
void show_quit_confirmation(void);
void show_reload_confirmation(int tab_index);
void reload_file_in_tab(int tab_index);

#endif
